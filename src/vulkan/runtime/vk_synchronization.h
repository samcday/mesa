/*
 * Copyright © 2023 Collabora, Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef VK_SYNCHRONIZATION_H
#define VK_SYNCHRONIZATION_H

#include <vulkan/vulkan_core.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Expands pipeline stage group flags
 *
 * Some stages like VK_PIPELINE_SHADER_STAGE_2_ALL_GRAPHICS_BIT represent more
 * than one stage.  This helper expands any such bits out to the full set of
 * individual stages bits they represent.
 */
VkPipelineStageFlags2
vk_expand_pipeline_stage_flags2(VkPipelineStageFlags2 stages);

/** Returns the set of read accesses allowed in the given stages */
VkAccessFlags2
vk_read_access2_for_pipeline_stage_flags2(VkPipelineStageFlags2 stages);

/** Returns the set of write accesses allowed in the given stages */
VkAccessFlags2
vk_write_access2_for_pipeline_stage_flags2(VkPipelineStageFlags2 stages);

#ifdef __cplusplus
}
#endif

#endif /* VK_SYNCHRONIZATION_H */