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

#include "defs.h"
#include "breakpoint.h"
#include "inferior.h"
#include "target.h"

#include "cuda-defs.h"
#include "cuda-iterator.h"
#include "cuda-state.h"

struct cuda_iterator_t
{
  cuda_iterator_type type;
  uint32_t num_elements;
  uint32_t num_unique_elements;
  uint32_t list_size;
  uint32_t next_index;
  cuda_coords_t current;
  cuda_coords_t *list;
};

/* Return a threads iterator sorted by coordinates. Entries must satisfy the
   filter and other arguments.  The iterator does not return any duplicate,
   although its internal implementation will have them. */
cuda_iterator
cuda_iterator_create (cuda_iterator_type type, cuda_coords_t *filter, cuda_select_t select_mask)
{
  uint32_t dev, sm, wp, ln, gridId, i;
  bool store_dev, store_sm, store_warp, store_lane, store_kernel, store_grid, store_block, store_thread;
  kernel_t kernel;
  uint64_t kernelId;
  CuDim3 blockIdx;
  CuDim3 threadIdx;
  cuda_coords_t *c;
  cuda_iterator itr;
  bool valid         = select_mask & CUDA_SELECT_VALID;
  bool at_breakpoint = select_mask & CUDA_SELECT_BKPT;
  bool at_exception  = select_mask & CUDA_SELECT_EXCPT;
  struct address_space *aspace = NULL;

  if (!ptid_equal (inferior_ptid, null_ptid))
    aspace = target_thread_address_space (inferior_ptid);

  itr = (cuda_iterator) xmalloc (sizeof *itr);
  itr->type         = type;
  itr->num_elements = 0;
  itr->num_unique_elements = 0;
  itr->list_size    = 1024;
  itr->next_index   = 0;
  itr->current      = CUDA_INVALID_COORDS;
  itr->list         = (cuda_coords_t*) xmalloc (itr->list_size * sizeof (*itr->list));

  /* only store information that can be uniquely identified given an object of the iterator type */
  store_dev    = (type & 0x0f) >= CUDA_ITERATOR_TYPE_DEVICES || (type & 0xf0) >= CUDA_ITERATOR_TYPE_KERNELS;
  store_sm     = (type & 0x0f) >= CUDA_ITERATOR_TYPE_SMS     || (type & 0xf0) >= CUDA_ITERATOR_TYPE_BLOCKS;
  store_warp   = (type & 0x0f) >= CUDA_ITERATOR_TYPE_WARPS   || (type & 0xf0) >= CUDA_ITERATOR_TYPE_THREADS;
  store_lane   = (type & 0x0f) >= CUDA_ITERATOR_TYPE_LANES   || (type & 0xf0) >= CUDA_ITERATOR_TYPE_THREADS;

  store_kernel = (type & 0x0f) >= CUDA_ITERATOR_TYPE_SMS     || (type & 0xf0) >= CUDA_ITERATOR_TYPE_KERNELS;
  store_grid   = (type & 0x0f) >= CUDA_ITERATOR_TYPE_SMS     || (type & 0xf0) >= CUDA_ITERATOR_TYPE_KERNELS;
  store_block  = (type & 0x0f) >= CUDA_ITERATOR_TYPE_WARPS   || (type & 0xf0) >= CUDA_ITERATOR_TYPE_BLOCKS;
  store_thread = (type & 0x0f) >= CUDA_ITERATOR_TYPE_LANES   || (type & 0xf0) >= CUDA_ITERATOR_TYPE_THREADS;

  /* populate the list with valid kernels meeting the specifications of the
     filter. Duplicates are dealt with later. */
  for (dev = 0; dev < cuda_system_get_num_devices (); ++dev)
    {
      if (valid && !device_is_valid (dev))
        continue;
      if (filter && filter->dev != CUDA_WILDCARD && filter->dev != dev)
        continue;

      for (sm = 0; sm < device_get_num_sms (dev); ++sm)
        {
          if (filter && filter->sm != CUDA_WILDCARD && filter->sm != sm)
            continue;

          for (wp = 0; wp < device_get_num_warps (dev); ++wp)
            {
              if (valid && !warp_is_valid (dev, sm, wp))
                continue;
              if (filter && filter->wp != CUDA_WILDCARD && filter->wp != wp)
                continue;

              if (warp_is_valid (dev, sm, wp))
                {
                  kernel   = warp_get_kernel (dev, sm, wp);
                  kernelId = kernel_get_id (kernel);
                  gridId   = kernel_get_grid_id (kernel);
                  blockIdx = warp_get_block_idx (dev, sm, wp);
                }
              else
                {
                  kernelId = CUDA_INVALID;
                  gridId   = CUDA_INVALID;
                  blockIdx = (CuDim3){ CUDA_INVALID, CUDA_INVALID, CUDA_INVALID };
                }

              if (filter &&
                  ((filter->kernelId   != CUDA_WILDCARD && filter->kernelId   != kernelId)   ||
                   (filter->gridId     != CUDA_WILDCARD && filter->gridId     != gridId)     ||
                   (filter->blockIdx.x != CUDA_WILDCARD && filter->blockIdx.x != blockIdx.x) ||
                   (filter->blockIdx.y != CUDA_WILDCARD && filter->blockIdx.y != blockIdx.y) ||
                   (filter->blockIdx.z != CUDA_WILDCARD && filter->blockIdx.z != blockIdx.z)))
                continue;

              for (ln = 0; ln < device_get_num_lanes (dev); ++ln)
                {
                  if (valid && !lane_is_valid (dev, sm, wp, ln))
                    continue;
                  if (filter && filter->ln != CUDA_WILDCARD && filter->ln != ln)
                    continue;

                  if (warp_is_valid (dev, sm, wp) &&
                      lane_is_valid (dev, sm, wp, ln))
                    threadIdx = lane_get_thread_idx (dev, sm, wp, ln);
                  else
                    threadIdx = (CuDim3){ CUDA_INVALID, CUDA_INVALID, CUDA_INVALID };

                  if (filter &&
                      ((filter->threadIdx.x != CUDA_WILDCARD && filter->threadIdx.x != threadIdx.x) ||
                       (filter->threadIdx.y != CUDA_WILDCARD && filter->threadIdx.y != threadIdx.y) ||
                       (filter->threadIdx.z != CUDA_WILDCARD && filter->threadIdx.z != threadIdx.z)))
                    continue;

                  /* if looking for breakpoints, skip non-broken kernels */
                  if (at_breakpoint &&
                      warp_is_valid (dev, sm, wp) &&
                      lane_is_valid (dev, sm, wp, ln) &&
                      !breakpoint_here_p (aspace, lane_get_virtual_pc (dev, sm, wp, ln)))
                    continue;

                  /* if looking for exceptions, skip healthy kernels */
                  if (at_exception &&
                      warp_is_valid (dev, sm, wp) &&
                      lane_is_valid (dev, sm, wp, ln) &&
                      !lane_get_exception (dev, sm, wp, ln))
                    continue;

                  /* allocate more memory if needed */
                  if (itr->num_elements >= itr->list_size)
                    {
                      itr->list_size *= 2;
                      itr->list = xrealloc (itr->list, itr->list_size * sizeof (*itr->list));
                    }

                  c = &itr->list[itr->num_elements];

                  *c = CUDA_WILDCARD_COORDS;
                  c->valid     = true;
                  c->dev       = store_dev    ? dev       : c->dev;
                  c->sm        = store_sm     ? sm        : c->sm;
                  c->wp        = store_warp   ? wp        : c->wp;
                  c->ln        = store_lane   ? ln        : c->ln;
                  c->kernelId  = store_kernel ? kernelId  : c->kernelId;
                  c->gridId    = store_grid   ? gridId    : c->gridId;
                  c->blockIdx  = store_block  ? blockIdx  : c->blockIdx;
                  c->threadIdx = store_thread ? threadIdx : c->threadIdx;

                  ++itr->num_elements;
                }
            }
        }
    }

  /* sort the list by coordinates */
  if (type & (CUDA_ITERATOR_TYPE_KERNELS | CUDA_ITERATOR_TYPE_BLOCKS | CUDA_ITERATOR_TYPE_THREADS))
    qsort (itr->list, itr->num_elements, sizeof (*itr->list),
           (int(*)(const void*, const void*))cuda_coords_compare_logical);
  else
    qsort (itr->list, itr->num_elements, sizeof (*itr->list),
           (int(*)(const void*, const void*))cuda_coords_compare_physical);

  /* Count the number of unique elements. The duplicates are not eliminated to
     save time. We can simply hop them when iterating. */
  if (itr->num_elements > 0)
    {
      itr->num_unique_elements = 1;
      for (i = 1; i < itr->num_elements; ++i)
        if (!cuda_coords_equal (&itr->list[i], &itr->list[i-1]))
            ++itr->num_unique_elements;
    }

  return itr;
}

cuda_iterator
cuda_iterator_destroy (cuda_iterator itr)
{
  xfree (itr->list);
  xfree (itr);
  return NULL;
}

cuda_iterator
cuda_iterator_start (cuda_iterator itr)
{
  itr->next_index = 0;
  itr = cuda_iterator_next (itr);
  return itr;
}

cuda_iterator
cuda_iterator_end (cuda_iterator itr)
{
  if (itr->current.valid)
    return NULL;
  else
    return itr;
}

cuda_iterator
cuda_iterator_next (cuda_iterator itr)
{
  if (itr->next_index >=  itr->num_elements)
    {
      itr->current.valid = false;
      return itr;
    }

  itr->current = itr->list[itr->next_index];

  ++itr->next_index;

  /* hop over the duplicate elements */
  while (itr->next_index < itr->num_elements &&
         cuda_coords_equal (&itr->list[itr->next_index],
                            &itr->list[itr->next_index-1]))
    ++itr->next_index;

  return itr;
}

cuda_coords_t
cuda_iterator_get_current (cuda_iterator itr)
{
  return itr->current;
}

uint32_t
cuda_iterator_get_size (cuda_iterator itr)
{
  return itr->num_unique_elements;
}