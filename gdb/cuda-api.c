/*
 * NVIDIA CUDA Debugger CUDA-GDB Copyright (C) 2007-2012 NVIDIA Corporation
 * Written by CUDA-GDB team at NVIDIA <cudatools@nvidia.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <execinfo.h>

#include "defs.h"
#include "inferior.h"
#include "gdb_assert.h"
#include "gdb_string.h"
#include "gdbcore.h"

#include "cuda-api.h"
#include "cuda-tdep.h"


static CUDBGAPI cudbgAPI = NULL;

static bool api_initialized = false;

int
cuda_api_get_api (void)
{
  CUDBGResult res;

  gdb_assert (!cudbgAPI);

  res = cudbgGetAPI (CUDBG_API_VERSION_MAJOR,
                     CUDBG_API_VERSION_MINOR,
                     CUDBG_API_VERSION_REVISION,
                     &cudbgAPI);

  switch (res)
    {
      case CUDBG_SUCCESS:
        return 0;

      case CUDBG_ERROR_INITIALIZATION_FAILURE:
        fprintf_unfiltered (gdb_stderr,
                            "The CUDA driver failed initialization. "
                            "Likely cause is X running on all devices.\n");
        break;

      default:
        fprintf_unfiltered (gdb_stderr,
                            "The CUDA Debugger API failed with error %d.\n",
                            res);
        break;
    }

  fprintf_unfiltered (gdb_stderr, "[CUDA Debugging is disabled]\n");
  return 1;
}

static void
cuda_api_fatal (const char *msg, CUDBGResult res)
{
  /* Finalize API */
  cudbgAPI->finalize ();

  /* Kill inferior */
  kill (cuda_gdb_get_tid (inferior_ptid), SIGKILL);

  /* Report error */
  fatal (_("fatal:  %s (error code = %d)"), msg, res);
}

int
cuda_api_initialize (void)
{
  CUDBGResult res;

  if (api_initialized)
    return 0;

  res = cudbgAPI->initialize ();

  switch (res)
    {
    case CUDBG_SUCCESS:
      api_initialized = true;
      return 0;

    case CUDBG_ERROR_SOME_DEVICES_WATCHDOGGED:
      warning (_("One or more CUDA devices are made unavailable to the application "
                 "because they are used for display and cannot be used while debugging. "
                 "This may change the application behavior."));
      api_initialized = true;
      return 0;

    case CUDBG_ERROR_UNINITIALIZED:
      /* Not ready yet. Will try later. */
      break;

    case CUDBG_ERROR_ALL_DEVICES_WATCHDOGGED:
      cuda_api_fatal ("All CUDA devices are used for display and cannot "
                              "be used while debugging.", res);
      break;

    case CUDBG_ERROR_INCOMPATIBLE_API:
      cuda_api_fatal ("Incompatible CUDA driver version.", res);
      break;

    case CUDBG_ERROR_INVALID_DEVICE:
      cuda_api_fatal ("One or more CUDA devices cannot be used for debugging. "
                      "Please consult the list of supported CUDA devices for more details.",
                      res);
    default:
      cuda_api_fatal ("The CUDA driver initialization failed.", res);
      break;
    }

  return 1;
}


void
cuda_api_finalize ()
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  /* Mark the API as not initialized as early as possible. If the finalize()
   * call fails, we won't try to do anything stupid afterwards. */
  api_initialized = false;

  res = cudbgAPI->finalize ();

  /* Only emit a warning in case of a failure, because cuda_api_finalize () can
     be called when an error occurs. That would create an infinite loop and/or
     undesired side effects. */
  if (res != CUDBG_SUCCESS)
    warning (_("Failed to finalize the CUDA debugger API (error=%u).\n"), res);
}

void
cuda_api_resume_device (uint32_t dev)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->resumeDevice (dev);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_RUNNING_DEVICE)
    error (_("Error: Failed to resume device (dev=%u, error=%u).\n"),
           dev, res);
}

void
cuda_api_suspend_device (uint32_t dev)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->suspendDevice (dev);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_SUSPENDED_DEVICE)
    error (_("Error: Failed to suspend device (dev=%u, error=%u).\n"),
           dev, res);
}

void
cuda_api_single_step_warp (uint32_t dev, uint32_t sm, uint32_t wp, uint64_t *warp_mask)
{
  CUDBGResult res;

  gdb_assert (warp_mask);

  if (!api_initialized)
    return;

  res = cudbgAPI->singleStepWarp (dev, sm, wp, warp_mask);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to single-step the warp "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

bool
cuda_api_set_breakpoint (uint32_t dev, uint64_t addr)
{
  CUDBGResult res;

  if (!api_initialized)
    return true;

  res = cudbgAPI->setBreakpoint (dev, addr);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_INVALID_ADDRESS)
    error (_("Error: Failed to set a breakpoint on device %u at address 0x%"PRIx64
             " (error=%u)."), dev, addr, res);
  return res != CUDBG_ERROR_INVALID_ADDRESS;
}

bool
cuda_api_unset_breakpoint (uint32_t dev, uint64_t addr)
{
  CUDBGResult res;

  if (!api_initialized)
    return true;

  res = cudbgAPI->unsetBreakpoint (dev, addr);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_INVALID_ADDRESS)
    error (_("Error: Failed to unset a breakpoint on device %u at address 0x%"PRIx64
             " (error=%u)."), dev, addr, res);
  return res != CUDBG_ERROR_INVALID_ADDRESS;
}

void
cuda_api_read_grid_id (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t *grid_id)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readGridId (dev, sm, wp, grid_id);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the grid index "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_read_block_idx (uint32_t dev, uint32_t sm, uint32_t wp, CuDim3 *blockIdx)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readBlockIdx (dev, sm, wp, blockIdx);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the block index "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_read_thread_idx (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, CuDim3 *threadIdx)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readThreadIdx (dev, sm, wp, ln, threadIdx);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the thread index "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_read_broken_warps (uint32_t dev, uint32_t sm, uint64_t *brokenWarpsMask)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readBrokenWarps (dev, sm, brokenWarpsMask);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the broken warps mask "
             "(dev=%u, sm=%u, error=%u).\n"),
           dev, sm, res);
}

void
cuda_api_read_valid_warps (uint32_t dev, uint32_t sm, uint64_t *valid_warps)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readValidWarps (dev, sm, valid_warps);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the valid warps mask "
             "(dev=%u, sm=%u, error=%u).\n"),
           dev, sm, res);
}

void
cuda_api_read_valid_lanes (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t *valid_lanes)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readValidLanes (dev, sm, wp, valid_lanes);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the valid lanes mask "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_read_active_lanes (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t *active_lanes)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readActiveLanes (dev, sm, wp, active_lanes);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the active lanes mask "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_read_code_memory (uint32_t dev, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readCodeMemory (dev, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read code memory at address 0x%"PRIx64
             " on device %u (error=%u)."), addr, dev, res);
}

void
cuda_api_read_const_memory (uint32_t dev, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readConstMemory (dev, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read const memory at address 0x%"PRIx64
             " on device %u (error=%u)."), addr, dev, res);
}

void
cuda_api_read_global_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;
  uint64_t hostaddr;

  if (!api_initialized)
    return;

  res = cudbgAPI->readGlobalMemory (dev, sm, wp, ln, addr, buf, sz);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_ADDRESS_NOT_IN_DEVICE_MEM)
    error (_("Error: Failed to read global memory at address 0x%"PRIx64
             " on device %u sm %u warp %u lane %u (error=%u)."), addr, dev, sm, wp, ln, res);

  if (res == CUDBG_ERROR_ADDRESS_NOT_IN_DEVICE_MEM)
    {
      res = cudbgAPI->getHostAddrFromDeviceAddr (dev, addr, &hostaddr);
      if (res != CUDBG_SUCCESS)
          error (_("Error:  Failed to translate device VA to host VA (error=%u)."), res);
      read_memory (hostaddr, buf, sz);
    }
}

bool
cuda_api_read_pinned_memory (uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return false;

  res = cudbgAPI->readPinnedMemory (addr, buf, sz);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_MEMORY_MAPPING_FAILED)
    error (_("Error: Failed to read pinned memory at address 0x%"PRIx64
             " (error=%u)."), addr, res);
  return res == CUDBG_SUCCESS;
}

void
cuda_api_read_param_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readParamMemory (dev, sm, wp, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read param memory at address 0x%"PRIx64
             " on device %u sm %u warp %u (error=%u)."), addr, dev, sm, wp, res);
}

void
cuda_api_read_shared_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readSharedMemory (dev, sm, wp, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read shared memory at address 0x%"PRIx64
             " on device %u sm %u warp %u (error=%u)."), addr, dev, sm, wp, res);
}

void
cuda_api_read_texture_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t id, uint32_t dim, uint32_t *coords, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readTextureMemory (dev, sm, wp, id, dim, coords, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read texture memory of texture %u dim %u coords %u"
             " on device %u sm %u warp %u (error=%u)."), id, dim, *coords, dev, sm, wp, res);
}

void
cuda_api_read_local_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t addr, void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readLocalMemory (dev, sm, wp, ln, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read local memory at address 0x%"PRIx64
             " on device %u sm %u warp %u lane %u (error=%u)."), addr, dev, sm, wp, ln, res);
}

void
cuda_api_read_register (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln,
                        uint32_t regno, uint32_t *val)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readRegister (dev, sm, wp, ln, regno, val);
  if (res != CUDBG_SUCCESS)
      error (_("Error: Failed to read register %d "
               "(dev=%u, sm=%u, wp=%u, ln=%u, error=%u).\n"),
             regno, dev, sm, wp, ln, res);
}

void
cuda_api_read_pc (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t *pc)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readPC (dev, sm, wp, ln, pc);
  if (res != CUDBG_SUCCESS)
    error (_("Failed to read the program counter on CUDA device %u (error%u).\n"), dev, res);
}

void
cuda_api_read_virtual_pc (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t *pc)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readVirtualPC (dev, sm, wp, ln, pc);
  if (res != CUDBG_SUCCESS)
    error (_("Failed to read the virtual PC on CUDA device %u (error=%u).\n"), dev, res);
}

void
cuda_api_read_lane_exception (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, CUDBGException_t *exception)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->readLaneException (dev, sm, wp, ln, exception);
  if (res != CUDBG_SUCCESS)
      error (_("Error: Failed to read the lane exception "
               "(dev=%u, sm=%u, wp=%u, ln=%u, error=%u).\n"),
             dev, sm, wp, ln, res);
}


void
cuda_api_read_call_depth (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, int32_t *depth)
{
  CUDBGResult res;
  uint32_t api_call_depth;

  if (!api_initialized)
    return;

  res = cudbgAPI->readCallDepth (dev, sm, wp, ln, &api_call_depth);
  if (res != CUDBG_SUCCESS)
    {
      error (_("Error: Could not read call depth "
               "(dev=%u, sm=%u, warp=%u, lane=%u, error=%u).\n"),
             dev, sm, wp, ln, res);
    }

  *depth = (int32_t) api_call_depth;
}

void
cuda_api_read_syscall_call_depth (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, int32_t *depth)
{
  CUDBGResult res;
  uint32_t api_syscall_call_depth;

  if (!api_initialized)
    return;

  res = cudbgAPI->readSyscallCallDepth (dev, sm, wp, ln, &api_syscall_call_depth);
  if (res != CUDBG_SUCCESS)
    {
      error (_("Error: Could not read syscall call depth "
               "(dev=%u, sm=%u, warp=%u, lane=%u, error=%u).\n"),
             dev, sm, wp, ln, res);
    }

  *depth = (int32_t) api_syscall_call_depth;
}

void
cuda_api_read_virtual_return_address (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln,
                                      int32_t level, uint64_t *ra)
{
  CUDBGResult res;
  uint32_t api_call_level;

  gdb_assert (level >= 0);

  api_call_level = (uint32_t) level;
  if (!api_initialized)
    return;

  res = cudbgAPI->readVirtualReturnAddress (dev, sm, wp, ln, api_call_level, ra);
  if (res != CUDBG_SUCCESS)
    {
      if (res == CUDBG_ERROR_INVALID_CALL_LEVEL)
        error (_("Error: Debugger API returned invalid call level for level %u."),
               api_call_level);
      else
        error (_("Error: Could not read virtual return address for level %u "
                 "(dev=%u, sm=%u, warp=%u, lane=%u, error=%u).\n"),
               api_call_level, dev, sm, wp, ln, res);
    }
}

void
cuda_api_write_global_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t addr, const void *buf, uint32_t sz)
{
  CUDBGResult res;
  uint64_t hostaddr;

  if (!api_initialized)
    return;

  res = cudbgAPI->writeGlobalMemory (dev, sm, wp, ln, addr, buf, sz);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_ADDRESS_NOT_IN_DEVICE_MEM)
    error (_("Error: Failed to write global memory at address 0x%"PRIx64
             " on device %u sm %u warp %u lane %u (error=%u)."), addr, dev, sm, wp, ln, res);

  if (res == CUDBG_ERROR_ADDRESS_NOT_IN_DEVICE_MEM)
    {
      res = cudbgAPI->getHostAddrFromDeviceAddr (dev, addr, &hostaddr);
      if (res != CUDBG_SUCCESS)
        error (_("Error:  Failed to translate device VA to host VA (error=%u)."), res);
      write_memory (hostaddr, buf, sz);
    }
}

bool
cuda_api_write_pinned_memory (uint64_t addr, const void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return false;

  res = cudbgAPI->writePinnedMemory (addr, buf, sz);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_MEMORY_MAPPING_FAILED)
    error (_("Error: Failed to write pinned memory at address 0x%"PRIx64
             " (error=%u)."), addr, res);
  return res == CUDBG_SUCCESS;
}

void
cuda_api_write_param_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint64_t addr, const void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->writeParamMemory (dev, sm, wp, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to write param memory at address 0x%"PRIx64
             " on device %u sm %u warp %u (error=%u)."), addr, dev, sm, wp, res);
}

void
cuda_api_write_shared_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint64_t addr, const void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->writeSharedMemory (dev, sm, wp, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to write shared memory at address 0x%"PRIx64
             " on device %u sm %u warp %u (error=%u)."), addr, dev, sm, wp, res);
}

void
cuda_api_write_local_memory (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint64_t addr, const void *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->writeLocalMemory (dev, sm, wp, ln, addr, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to write local memory at address 0x%"PRIx64
             " on device %u sm %u warp %u lane %u (error=%u)."), addr, dev, sm, wp, ln, res);
}

void
cuda_api_write_register (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t ln, uint32_t regno, uint32_t val)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->writeRegister (dev, sm, wp, ln, regno, val);
  if (res != CUDBG_SUCCESS)
      error (_("Error: Failed to write register %d "
               "(dev=%u, sm=%u, wp=%u, ln=%u, error=%u).\n"),
             regno, dev, sm, wp, ln, res);
}

void
cuda_api_get_grid_dim (uint32_t dev, uint32_t sm, uint32_t wp, CuDim3 *grid_dim)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getGridDim (dev, sm, wp, grid_dim);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the grid dimensions "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_get_block_dim (uint32_t dev, uint32_t sm, uint32_t wp, CuDim3 *block_dim)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getBlockDim (dev, sm, wp, block_dim);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the block dimensions "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

 void
cuda_api_get_blocking (uint32_t dev, uint32_t sm, uint32_t wp, bool *blocking)
{
  CUDBGResult res;
  uint64_t blocking64;

  if (!api_initialized)
    return;

  res = cudbgAPI->getGridAttribute (dev, sm, wp, CUDBG_ATTR_GRID_LAUNCH_BLOCKING, &blocking64);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the grid blocking attribute "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);

  *blocking = !!blocking64;
}

void
cuda_api_get_tid (uint32_t dev, uint32_t sm, uint32_t wp, uint32_t *tid)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getTID (dev, sm, wp, tid);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to thread id "
             "(dev=%u, sm=%u, wp=%u, error=%u).\n"),
           dev, sm, wp, res);
}

void
cuda_api_get_elf_image (uint32_t dev, uint32_t sm, uint32_t wp, bool relocated,
                        void **elfImage, uint64_t *size)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getElfImage (dev, sm, wp, relocated, elfImage, size);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to read the ELF image "
             "(dev=%u, sm=%u, wp=%u, relocated=%d, error=%u).\n"),
           dev, sm, wp, relocated, res);
}

void
cuda_api_get_device_type (uint32_t dev, char *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getDeviceType (dev, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the device type"
             "(dev=%u, error=%u).\n"), dev, res);
}

void
cuda_api_get_sm_type (uint32_t dev, char *buf, uint32_t sz)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getSmType (dev, buf, sz);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the SM type"
             "(dev=%u, error=%u).\n"), dev, res);
}

void
cuda_api_get_num_devices (uint32_t *numDev)
{
  CUDBGResult res;

  *numDev = 0;

  if (!api_initialized)
    return;

  res = cudbgAPI->getNumDevices (numDev);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the number of devices"
             "(error=%u).\n"), res);
}

void
cuda_api_get_num_sms (uint32_t dev, uint32_t *numSMs)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getNumSMs (dev, numSMs);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the number of SMs"
             "(dev=%u, error=%u).\n"), dev, res);
}

void
cuda_api_get_num_warps (uint32_t dev, uint32_t *numWarps)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getNumWarps (dev, numWarps);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the number of warps"
             "(dev=%u, error=%u).\n"), dev, res);
}

void
cuda_api_get_num_lanes (uint32_t dev, uint32_t *numLanes)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getNumLanes (dev, numLanes);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the number of lanes"
             "(dev=%u, error=%u).\n"), dev, res);
}

void
cuda_api_get_num_registers (uint32_t dev, uint32_t *numRegs)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->getNumRegisters (dev, numRegs);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to get the number of registers"
             "(dev=%u, error=%u).\n"), dev, res);
}


void
cuda_api_is_device_code_address (uint64_t addr, bool *is_device_address)
{
  CUDBGResult res;

  if (!api_initialized)
    {
      *is_device_address = false;
      return;
    }

  if (!api_initialized)
    return;

  res = cudbgAPI->isDeviceCodeAddress (addr, is_device_address);
  if (res != CUDBG_SUCCESS)
      error (_("Error: Failed to determine if address 0x%"PRIx64" corresponds "
               "to the host or device (error=%u). "), addr, res);
}

bool
cuda_api_lookup_device_code_symbol (char *name, uint64_t *addr)
{
  bool found;
  CUDBGResult res;

  found = false;

  if (!api_initialized)
    return false;

  res = cudbgAPI->lookupDeviceCodeSymbol (name, &found, (uintptr_t *) addr);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to find address for device symbol %s (error=%u)."),
           name, res);

  return found;
}

void
cuda_api_set_notify_new_event_callback (CUDBGNotifyNewEventCallback callback)
{
  CUDBGResult res;

  /* Nothing should restrict the callback from being setup.
     In particular, it must be done prior to the API being
     fully initialized, which means there should not be a
     check here. */

  res = cudbgAPI->setNotifyNewEventCallback (callback);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to set the new event callback (error=%u)."), res);
}

void
cuda_api_get_next_event (CUDBGEvent *event)
{
  CUDBGResult res;

  event->kind = CUDBG_EVENT_INVALID;
  if (!api_initialized)
    return;

  res = cudbgAPI->getNextEvent (event);
  if (res != CUDBG_SUCCESS && res != CUDBG_ERROR_NO_EVENT_AVAILABLE)
    error (_("Error: Failed to get the next CUDA event (error=%u)."), res);
}

void
cuda_api_acknowledge_events (void)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->acknowledgeEvents ();
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to acknowledge a CUDA event (error=%u)."), res);
}

void
cuda_api_disassemble (uint32_t dev, uint64_t addr, uint32_t *instSize, char *buf, uint32_t bufSize)
{
  CUDBGResult res;

  if (!api_initialized)
    return;

  res = cudbgAPI->disassemble (dev, addr, instSize, buf, bufSize);
  if (res != CUDBG_SUCCESS)
    error (_("Error: Failed to disassemble instruction at address 0x%"PRIx64
             " on CUDA device %u (error=%u)."), addr, dev, res);
}