#include "nvk_query_pool.h"

#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "util/os_time.h"

#include "nouveau_bo.h"

#include "nvk_cl9097.h"
#include "nvk_cl906f.h"
#include "nvk_cla0c0.h"

struct nvk_query_report {
   uint64_t value;
   uint64_t timestamp;
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateQueryPool(VkDevice device,
                    const VkQueryPoolCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_query_pool *pool;

   pool = vk_query_pool_create(&dev->vk, pCreateInfo,
                               pAllocator, sizeof(*pool));
   if (!pool)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* We place the availability first and then data */
   pool->query_start = ALIGN_POT(pool->vk.query_count * sizeof(uint32_t),
                                 sizeof(struct nvk_query_report));

   uint32_t reports_per_query;
   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      reports_per_query = 2;
      break;
   case VK_QUERY_TYPE_TIMESTAMP:
      reports_per_query = 1;
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      reports_per_query = 2 * util_bitcount(pool->vk.pipeline_statistics);
      break;
   default:
      unreachable("Unsupported query type");
   }
   pool->query_stride = reports_per_query * sizeof(struct nvk_query_report);

   if (pool->vk.query_count > 0) {
      uint32_t bo_size = pool->query_start +
                         pool->query_stride * pool->vk.query_count;
      pool->bo = nouveau_ws_bo_new_mapped(dev->pdev->dev, bo_size, 0,
                                          NOUVEAU_WS_BO_GART,
                                          NOUVEAU_WS_BO_RDWR,
                                          &pool->bo_map);
      if (!pool->bo) {
         vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }

      if (dev->pdev->dev->debug_flags & NVK_DEBUG_ZERO_MEMORY)
         memset(pool->bo_map, 0, bo_size);
   }

   *pQueryPool = nvk_query_pool_to_handle(pool);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyQueryPool(VkDevice device,
                     VkQueryPool queryPool,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   if (!pool)
      return;

   if (pool->bo) {
      nouveau_ws_bo_unmap(pool->bo, pool->bo_map);
      nouveau_ws_bo_destroy(pool->bo);
   }
   vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
}

static uint64_t
nvk_query_available_addr(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return pool->bo->offset + query * sizeof(uint32_t);
}

static uint32_t *
nvk_query_available_map(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return (uint32_t *)pool->bo_map + query;
}

static uint64_t
nvk_query_offset(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return pool->query_start + query * pool->query_stride;
}

static uint64_t
nvk_query_report_addr(struct nvk_query_pool *pool, uint32_t query)
{
   return pool->bo->offset + nvk_query_offset(pool, query);
}

static struct nvk_query_report *
nvk_query_report_map(struct nvk_query_pool *pool, uint32_t query)
{
   return (void *)((char *)pool->bo_map + nvk_query_offset(pool, query));
}

VKAPI_ATTR void VKAPI_CALL
nvk_ResetQueryPool(VkDevice device,
                   VkQueryPool queryPool,
                   uint32_t firstQuery,
                   uint32_t queryCount)
{
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   uint32_t *available = nvk_query_available_map(pool, firstQuery);
   memset(available, 0, queryCount * sizeof(*available));
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                      VkQueryPool queryPool,
                      uint32_t firstQuery,
                      uint32_t queryCount)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   nvk_cmd_buffer_ref_bo(cmd, pool->bo);

   for (uint32_t i = 0; i < queryCount; i++) {
      uint64_t addr = nvk_query_available_addr(pool, firstQuery + i);

      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, 0);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .structure_size = STRUCTURE_SIZE_ONE_WORD,
      });
   }

   /* Wait for the above writes to complete.  This prevents WaW hazards on any
    * later query availability updates and ensures vkCmdCopyQueryPoolResults
    * will see the query as unavailable if it happens before the query is
    * completed again.
    */
   for (uint32_t i = 0; i < queryCount; i++) {
      uint64_t addr = nvk_query_available_addr(pool, firstQuery + i);

      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      __push_mthd(p, SUBC_NV9097, NV906F_SEMAPHOREA);
      P_NV906F_SEMAPHOREA(p, addr >> 32);
      P_NV906F_SEMAPHOREB(p, (addr & UINT32_MAX) >> 2);
      P_NV906F_SEMAPHOREC(p, 0);
      P_NV906F_SEMAPHORED(p, {
         .operation = OPERATION_ACQUIRE,
         .acquire_switch = ACQUIRE_SWITCH_ENABLED,
         .release_size = RELEASE_SIZE_4BYTE,
      });
   }
}

static bool
clear_bits64(uint64_t *bitfield, uint64_t bits)
{
   bool has_bits = (*bitfield & bits) != 0;
   *bitfield &= ~bits;
   return has_bits;
}

static uint32_t
vk_stage_flags_to_nv9097_pipeline_location(VkPipelineStageFlags2 flags)
{
   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR |
                            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                            VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|
                            VK_PIPELINE_STAGE_2_COPY_BIT|
                            VK_PIPELINE_STAGE_2_RESOLVE_BIT|
                            VK_PIPELINE_STAGE_2_BLIT_BIT|
                            VK_PIPELINE_STAGE_2_CLEAR_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_ALL;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_DEPTH_TEST;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_PIXEL_SHADER;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_ZCULL;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_STREAMING_OUTPUT;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_GEOMETRY_SHADER;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_TESSELATION_SHADER;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_TESSELATION_INIT_SHADER;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_VERTEX_SHADER;

   if (clear_bits64(&flags, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT))
      return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_DATA_ASSEMBLER;

   clear_bits64(&flags, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT |
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                        VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT);

   /* TODO: Doing this on 3D will likely cause a WFI which is probably ok but,
    * if we tracked which subchannel we've used most recently, we can probably
    * do better than that.
    */
   clear_bits64(&flags, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

   assert(flags == 0);

   return NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_NONE;
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdWriteTimestamp2(VkCommandBuffer commandBuffer,
                       VkPipelineStageFlags2 stage,
                       VkQueryPool queryPool,
                       uint32_t query)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   nvk_cmd_buffer_ref_bo(cmd, pool->bo);

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 10);

   uint64_t report_addr = nvk_query_report_addr(pool, query);
   P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
   P_NV9097_SET_REPORT_SEMAPHORE_A(p, report_addr >> 32);
   P_NV9097_SET_REPORT_SEMAPHORE_B(p, report_addr);
   P_NV9097_SET_REPORT_SEMAPHORE_C(p, 0);
   P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
      .operation = OPERATION_REPORT_ONLY,
      .pipeline_location = vk_stage_flags_to_nv9097_pipeline_location(stage),
      .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
   });

   uint64_t available_addr = nvk_query_available_addr(pool, query);
   P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
   P_NV9097_SET_REPORT_SEMAPHORE_A(p, available_addr >> 32);
   P_NV9097_SET_REPORT_SEMAPHORE_B(p, available_addr);
   P_NV9097_SET_REPORT_SEMAPHORE_C(p, 1);
   P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
      .operation = OPERATION_RELEASE,
      .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
      .pipeline_location = PIPELINE_LOCATION_ALL,
      .structure_size = STRUCTURE_SIZE_ONE_WORD,
   });
}

struct nvk_3d_stat_query {
   VkQueryPipelineStatisticFlagBits flag;
   uint8_t loc;
   uint8_t report;
};

/* This must remain sorted in flag order */
static const struct nvk_3d_stat_query nvk_3d_stat_queries[] = {{
   .flag    = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_DATA_ASSEMBLER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_DA_VERTICES_GENERATED,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_DATA_ASSEMBLER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_DA_PRIMITIVES_GENERATED,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_VERTEX_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_VS_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_GEOMETRY_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_GS_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_GEOMETRY_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_GS_PRIMITIVES_GENERATED,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_VPC, /* TODO */
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_CLIPPER_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_VPC, /* TODO */
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_CLIPPER_PRIMITIVES_GENERATED,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_PIXEL_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_PS_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_TESSELATION_INIT_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_TI_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT,
   .loc     = NV9097_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_TESSELATION_SHADER,
   .report  = NV9097_SET_REPORT_SEMAPHORE_D_REPORT_TS_INVOCATIONS,
}, {
   .flag    = VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT,
   .loc     = UINT8_MAX,
   .report  = UINT8_MAX,
}};

static void
nvk_cmd_begin_end_query(struct nvk_cmd_buffer *cmd,
                        struct nvk_query_pool *pool,
                        uint32_t query, uint32_t index,
                        bool end)
{
   uint64_t report_addr = nvk_query_report_addr(pool, query) +
                          end * sizeof(struct nvk_query_report);

   struct nv_push *p;
   switch (pool->vk.query_type) {
   case VK_QUERY_TYPE_OCCLUSION:
      p = nvk_cmd_buffer_push(cmd, 2 + 5 * (1 + end));

      P_IMMD(p, NV9097, SET_ZPASS_PIXEL_COUNT, !end);

      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, report_addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, report_addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, 0);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_REPORT_ONLY,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .report = REPORT_ZPASS_PIXEL_CNT64,
         .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
      });
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS: {
      uint32_t stat_count = util_bitcount(pool->vk.pipeline_statistics);
      p = nvk_cmd_buffer_push(cmd, (stat_count + end) * 5);

      ASSERTED uint32_t stats_left = pool->vk.pipeline_statistics;
      for (uint32_t i = 0; i < ARRAY_SIZE(nvk_3d_stat_queries); i++) {
         const struct nvk_3d_stat_query *sq = &nvk_3d_stat_queries[i];
         if (!(stats_left & sq->flag))
            continue;

         /* The 3D stat queries array MUST be sorted */
         assert(!(stats_left & (sq->flag - 1)));

         P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
         P_NV9097_SET_REPORT_SEMAPHORE_A(p, report_addr >> 32);
         P_NV9097_SET_REPORT_SEMAPHORE_B(p, report_addr);
         P_NV9097_SET_REPORT_SEMAPHORE_C(p, 0);
         P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
            .operation = OPERATION_REPORT_ONLY,
            .pipeline_location = sq->loc,
            .report = sq->report,
            .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
         });

         report_addr += 2 * sizeof(struct nvk_query_report);
         stats_left &= ~sq->flag;
      }
      break;
   }
   default:
      unreachable("Unsupported query type");
   }

   if (end) {
      uint64_t available_addr = nvk_query_available_addr(pool, query);
      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, available_addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, available_addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, 1);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .structure_size = STRUCTURE_SIZE_ONE_WORD,
      });
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer,
                            VkQueryPool queryPool,
                            uint32_t query,
                            VkQueryControlFlags flags,
                            uint32_t index)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   nvk_cmd_buffer_ref_bo(cmd, pool->bo);

   nvk_cmd_begin_end_query(cmd, pool, query, index, false);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer,
                          VkQueryPool queryPool,
                          uint32_t query,
                          uint32_t index)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   nvk_cmd_buffer_ref_bo(cmd, pool->bo);

   nvk_cmd_begin_end_query(cmd, pool, query, index, true);
}

static bool
nvk_query_is_available(struct nvk_query_pool *pool, uint32_t query)
{
   uint32_t *available = nvk_query_available_map(pool, query);
   return p_atomic_read(available) != 0;
}

#define NVK_QUERY_TIMEOUT 2000000000ull

static VkResult
nvk_query_wait_for_available(struct nvk_device *dev,
                             struct nvk_query_pool *pool,
                             uint32_t query)
{
   uint64_t abs_timeout_ns = os_time_get_absolute_timeout(NVK_QUERY_TIMEOUT);

   while (os_time_get_nano() < abs_timeout_ns) {
      if (nvk_query_is_available(pool, query))
         return VK_SUCCESS;

      VkResult status = vk_device_check_status(&dev->vk);
      if (status != VK_SUCCESS)
         return status;
   }

   return vk_device_set_lost(&dev->vk, "query timeout");
}

static void
cpu_write_query_result(void *dst, uint32_t idx,
                       VkQueryResultFlags flags,
                       uint64_t result)
{
   if (flags & VK_QUERY_RESULT_64_BIT) {
      uint64_t *dst64 = dst;
      dst64[idx] = result;
   } else {
      uint32_t *dst32 = dst;
      dst32[idx] = result;
   }
}

static void
cpu_get_query_delta(void *dst, const struct nvk_query_report *src,
                    uint32_t idx, VkQueryResultFlags flags)
{
   uint64_t delta = src[idx * 2 + 1].value - src[idx * 2].value;
   cpu_write_query_result(dst, idx, flags, delta);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetQueryPoolResults(VkDevice device,
                        VkQueryPool queryPool,
                        uint32_t firstQuery,
                        uint32_t queryCount,
                        size_t dataSize,
                        void *pData,
                        VkDeviceSize stride,
                        VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   if (vk_device_is_lost(&dev->vk))
      return VK_ERROR_DEVICE_LOST;

   VkResult status = VK_SUCCESS;
   for (uint32_t i = 0; i < queryCount; i++) {
      const uint32_t query = firstQuery + i;

      bool available = nvk_query_is_available(pool, query);

      if (!available && (flags & VK_QUERY_RESULT_WAIT_BIT)) {
         status = nvk_query_wait_for_available(dev, pool, query);
         if (status != VK_SUCCESS)
            return status;

         available = true;
      }

      bool write_results = available || (flags & VK_QUERY_RESULT_PARTIAL_BIT);

      const struct nvk_query_report *src = nvk_query_report_map(pool, query);
      assert(i * stride < dataSize);
      void *dst = (char *)pData + i * stride;

      uint32_t available_dst_idx = 1;
      switch (pool->vk.query_type) {
      case VK_QUERY_TYPE_OCCLUSION:
         if (write_results)
            cpu_get_query_delta(dst, src, 0, flags);
         break;
      case VK_QUERY_TYPE_PIPELINE_STATISTICS: {
         uint32_t stat_count = util_bitcount(pool->vk.pipeline_statistics);
         available_dst_idx = stat_count;
         if (write_results) {
            for (uint32_t j = 0; j < stat_count; j++)
               cpu_get_query_delta(dst, src, j, flags);
         }
         break;
      }
      case VK_QUERY_TYPE_TIMESTAMP:
         if (write_results)
            cpu_write_query_result(dst, 0, flags, src->timestamp);
         break;
      default:
         unreachable("Unsupported query type");
      }

      if (!write_results)
         status = VK_NOT_READY;

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
         cpu_write_query_result(dst, available_dst_idx, flags, available);
   }

   return status;
}