#include "../rps_core.h"
#include "../rps_palloc.h"
void test_alignment(rps_pool_t *pool) {
    for (int i = 1; i < 20; i++) {
        void *p = rps_palloc(pool, i);
        if (((uintptr_t)p & 7) != 0) {
            printf("Alignment Error at size %d: address %p\n", i, p);
        }
    }
}

void test_growth(rps_pool_t *pool) {
    for (int i = 0; i < 1000; i++) {
        // 每次分配一个刚好能让每个 Block 放不下的尺寸
        rps_palloc(pool, 512); 
    }
    if(pool->current != pool) {
        printf("Pool growth test passed: current block moved to next\n");
    } else {
        printf("Pool growth test failed: current block did not move\n");
    }
    // 观察 pool->current 是否移动到了链表后方
}

void test_large(rps_pool_t *pool) {
    void *p1 = rps_palloc(pool, pool->max + 1); // 刚好触发大块
    void *p2 = rps_palloc(pool, 1024 * 1024);   // 1MB
    // 验证逻辑：检查 pool->large 是否有两个节点
    if(pool->large == NULL || pool->large->next == NULL) {
        printf("Large allocation failed\n");
    }
}


int main()
{
    rps_pool_t * pool = rps_create_pool(1024);
    test_alignment(pool);
    test_growth(pool);
    test_large(pool);

    rps_destroy_pool(pool);
    return 0;
}