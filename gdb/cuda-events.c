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

#include "defs.h"
#include "inferior.h"
#ifdef __linux__
#include "linux-nat.h"
#endif
#include "source.h"
#include "target.h"
#include "arch-utils.h"

#include "cuda-context.h"
#include "cuda-events.h"
#include "cuda-kernel.h"
#include "cuda-modules.h"
#include "cuda-options.h"
#include "cuda-state.h"
#include "cuda-tdep.h"
#include "cuda-modules.h"

static void
cuda_event_create_context (uint32_t dev_id, uint64_t context_id, uint32_t tid)
{
  contexts_t contexts;
  context_t  context;

  cuda_trace ("CUDBG_EVENT_CTX_CREATE dev_id=%u context=%"PRIx64" tid=%u",
              dev_id, context_id, tid);

  if (tid == ~0U)
    error (_("A CUDA event reported an invalid thread id."));

  contexts = device_get_contexts (dev_id);
  context  = context_new (context_id, dev_id);

  contexts_add_context (contexts, context);
  contexts_stack_context (contexts, context, tid);

  if (cuda_options_show_context_events ())
    printf_unfiltered (_("[Context Create of context 0x%"PRIx64" on Device %u]\n"),
                       context_id, dev_id);
}

static void
cuda_event_destroy_context (uint32_t dev_id, uint64_t context_id, uint32_t tid)
{
  contexts_t contexts;
  context_t  context;

  cuda_trace ("CUDBG_EVENT_CTX_DESTROY dev_id=%u context=%"PRIx64" tid=%u",
              dev_id, context_id);

  if (tid == ~0U)
    error (_("A CUDA event reported an invalid thread id."));

  contexts = device_get_contexts (dev_id);
  context  = contexts_find_context_by_id (contexts, context_id);

  if (contexts_get_active_context (contexts, tid) == context)
    context = contexts_unstack_context (contexts, tid);

  if (context == get_current_context ())
    set_current_context (NULL);

  cuda_cleanup_auto_breakpoints (&context_id);
  cuda_unresolve_breakpoints (context_id);

  contexts_remove_context (contexts, context);
  context_delete (context);

  if (cuda_options_show_context_events ())
    printf_unfiltered (_("[Context Destroy of context 0x%"PRIx64" on Device %u]\n"),
                       context_id, dev_id);
}

static void
cuda_event_push_context (uint32_t dev_id, uint64_t context_id, uint32_t tid)
{
  contexts_t contexts;
  context_t  context;

  cuda_trace ("CUDBG_EVENT_CTX_PUSH dev_id=%u context=%"PRIx64" tid=%u",
              dev_id, context_id);

  if (tid == ~0U)
    error (_("A CUDA event reported an invalid thread id."));

  contexts = device_get_contexts (dev_id);
  context  = contexts_find_context_by_id (contexts, context_id);

  contexts_stack_context (contexts, context, tid);

  if (cuda_options_show_context_events ())
    printf_unfiltered (_("[Context Push of context 0x%"PRIx64" on Device %u]\n"),
                       context_id, dev_id);
}

static void
cuda_event_pop_context (uint32_t dev_id, uint64_t context_id, uint32_t tid)
{
  contexts_t contexts;
  context_t  context;

  cuda_trace ("CUDBG_EVENT_CTX_POP dev_id=%u context=%"PRIx64" tid=%u",
              dev_id, context_id);

  if (tid == ~0U)
    error (_("A CUDA event reported an invalid thread id."));

  contexts = device_get_contexts (dev_id);
  context  = contexts_unstack_context (contexts, tid);

  gdb_assert (context_get_id (context) == context_id);

  if (cuda_options_show_context_events ())
    printf_unfiltered (_("[Context Pop of context 0x%"PRIx64" on Device %u]\n"),
                       context_id, dev_id);
}

static void
cuda_event_load_elf_image (uint32_t dev_id, uint64_t context_id, uint64_t module_id,
                           void *elf_image, uint64_t elf_image_size)
{
  context_t  context;
  modules_t  modules;
  module_t   module;

  cuda_trace ("CUDBG_EVENT_ELF_IMAGE_LOADED dev_id=%u context=%"PRIx64
              " module=%"PRIx64, dev_id, context_id, module_id);

  context = device_find_context_by_id (dev_id, context_id);
  modules = context_get_modules (context);
  module  = module_new (context, module_id, elf_image, elf_image_size);
  modules_add (modules, module);

  set_current_context (context);

#ifdef cuda_mark_breakpoints_as_unset
  /* CUDA - Due to an APPLE local optimization, line # breakpoints
     that need to be converted to a device address will not resolve
     unless we explicitly mark them as not being set */
  cuda_mark_breakpoints_as_unset ();
#endif

  /* Try to resolve any pending breakpoints now that we have a new module
     loaded */
  cuda_resolve_breakpoints (module_get_elf_image (module));
}

#ifdef __linux__
static int
find_lwp_callback (struct lwp_info *lp, void *data)
{
  uint32_t tid = *(uint32_t*)data;

  gdb_assert (cuda_platform_supports_tid ());

  return cuda_gdb_get_tid (lp->ptid) == tid;
}
#endif

static void
cuda_event_kernel_ready (uint32_t dev_id, uint64_t context_id, uint64_t module_id,
                         uint32_t grid_id, uint32_t tid, uint64_t virt_code_base,
                         CuDim3 grid_dim, CuDim3 block_dim, CUDBGKernelType type)
{
  kernels_t        kernels;
  ptid_t           previous_ptid = inferior_ptid;
#if __linux__
  struct lwp_info *lp            = NULL;
#endif
  struct gdbarch  *gdbarch       = get_current_arch ();

  cuda_trace ("CUDBG_EVENT_KERNEL_READY dev_id=%u context=%"PRIx64
              " module=%"PRIx64" grid_id=%u tid=%u type=%u\n",
              dev_id, context_id, module_id, grid_id, tid, type);

  if (tid == ~0U)
    error (_("A CUDA event reported an invalid thread id."));

#if __linux__
  //FIXME - CUDA MAC OS X
  lp = iterate_over_lwps (inferior_ptid, find_lwp_callback, &tid);

  if (lp)
    {
      previous_ptid = inferior_ptid;
      inferior_ptid = lp->ptid;
    }
#endif

  kernels = device_get_kernels (dev_id);
  kernels_start_kernel (kernels, grid_id, virt_code_base, context_id,
                        module_id, grid_dim, block_dim, type);

  if ((type == CUDBG_KNL_TYPE_APPLICATION &&
       cuda_options_break_on_launch_application ()) ||
      (type == CUDBG_KNL_TYPE_SYSTEM &&
       cuda_options_break_on_launch_system ()))
    cuda_create_auto_breakpoint (virt_code_base, context_id);

  remove_breakpoints ();
  insert_breakpoints ();

#if __linux__
  if (lp)
    inferior_ptid = previous_ptid;
#endif
}

static void
cuda_event_kernel_finished (uint32_t dev_id, uint32_t grid_id)
{
  kernels_t kernels;
  kernel_t  kernel;

  cuda_trace ("CUDBG_EVENT_KERNEL_FINISHED dev_id=%u grid_id=%u\n",
              dev_id, grid_id);

  /* No kernel if cuda_kernel_update already captured the kernel termination. */
  kernels = device_get_kernels (dev_id);
  kernel  = kernels_find_kernel_by_grid_id (kernels, grid_id);
  if (!kernel)
    return;

  kernels_terminate_kernel (kernels, kernel);

  clear_current_source_symtab_and_line ();
  clear_displays ();
}

static void
cuda_event_error (void)
{
  cuda_trace ("CUDBG_EVENT_ERROR\n");

  cuda_cleanup ();
  kill (cuda_gdb_get_tid (inferior_ptid), SIGKILL);

  error (_("Error: Unexpected error reported by the CUDA debugger API. "
          "Session is now unstable."));
}

static void
cuda_event_timeout (void)
{
  cuda_trace ("CUDBG_EVENT_TIMEOUT\n");
}

void
cuda_process_events (CUDBGEvent *event)
{
  uint32_t dev_id;
  uint32_t grid_id;
  uint32_t tid;
  uint64_t virt_code_base;
  uint64_t context_id;
  uint64_t module_id;
  uint64_t elf_image_size;
  void    *elf_image;
  CuDim3   grid_dim;
  CuDim3   block_dim;
  CUDBGKernelType type;

  gdb_assert (event);

  for (; event->kind != CUDBG_EVENT_INVALID; cuda_api_get_next_event (event))
    {
      switch (event->kind)
        {
        case CUDBG_EVENT_ELF_IMAGE_LOADED:
          {
            dev_id         = event->cases.elfImageLoaded.dev;
            context_id     = event->cases.elfImageLoaded.context;
            module_id      = event->cases.elfImageLoaded.module;
            elf_image      = event->cases.elfImageLoaded.relocatedElfImage;
            elf_image_size = event->cases.elfImageLoaded.size;
            cuda_event_load_elf_image (dev_id, context_id, module_id,
                                       elf_image, elf_image_size);
            break;
          }
        case CUDBG_EVENT_KERNEL_READY:
          {
            dev_id         = event->cases.kernelReady.dev;
            context_id     = event->cases.kernelReady.context;
            module_id      = event->cases.kernelReady.module;
            grid_id        = event->cases.kernelReady.gridId;
            tid            = event->cases.kernelReady.tid;
            virt_code_base = event->cases.kernelReady.functionEntry;
            grid_dim       = event->cases.kernelReady.gridDim;
            block_dim      = event->cases.kernelReady.blockDim;
            type           = event->cases.kernelReady.type;
            cuda_event_kernel_ready (dev_id, context_id, module_id, grid_id,
                                     tid, virt_code_base, grid_dim, block_dim,
                                     type);
            break;
          }
        case CUDBG_EVENT_KERNEL_FINISHED:
          {
            dev_id  = event->cases.kernelFinished.dev;
            grid_id = event->cases.kernelFinished.gridId;
            cuda_event_kernel_finished (dev_id, grid_id);
            break;
          }
        case CUDBG_EVENT_CTX_PUSH:
          {
            dev_id     = event->cases.contextPush.dev;
            context_id = event->cases.contextPush.context;
            tid        = event->cases.contextPush.tid;
            cuda_event_push_context (dev_id, context_id, tid);
            break;
          }
        case CUDBG_EVENT_CTX_POP:
          {
            dev_id     = event->cases.contextPop.dev;
            context_id = event->cases.contextPop.context;
            tid        = event->cases.contextPop.tid;
            cuda_event_pop_context (dev_id, context_id, tid);
            break;
          }
        case CUDBG_EVENT_CTX_CREATE:
          {
            dev_id     = event->cases.contextCreate.dev;
            context_id = event->cases.contextCreate.context;
            tid        = event->cases.contextCreate.tid;
            cuda_event_create_context (dev_id, context_id, tid);
            break;
          }
        case CUDBG_EVENT_CTX_DESTROY:
          {
            dev_id     = event->cases.contextDestroy.dev;
            context_id = event->cases.contextDestroy.context;
            tid        = event->cases.contextDestroy.tid;
            cuda_event_destroy_context (dev_id, context_id, tid);
            break;
          }
        case CUDBG_EVENT_ERROR:
          {
            cuda_event_error ();
            break;
          }
        case CUDBG_EVENT_TIMEOUT:
          {
            cuda_event_timeout ();
            break;
          }
        default:
          gdb_assert (0);
        }
    }
}
