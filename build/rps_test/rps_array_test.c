#include "../rps_array.h"
#include "../rps_core.h"
#include "../rps_palloc.h"

typedef struct{
    char *name;
    int num;
}test;

int main(){
    rps_pool_t * pool = rps_create_pool(520);
    rps_array_t * array =rps_array_create(pool, 10,sizeof(test));
    printf("sizeof(struct test)==%lu\n",sizeof(test));
    printf("sizeof(struct rps_pool_t)==%lu\n",sizeof(rps_pool_t));
    printf("sizeof(struct array_control_block)==%lu\n",sizeof(rps_array_t));
    printf("pool->d.next == %p\n",pool->d.next);

        printf("pool->large==%p\n",pool->large);

    for(int i=0;i<40;i++)
    {
        test * data = rps_array_push(array);
        printf("%p ",array->elts);
        data->name = NULL;
        data->num  = i;
    } 
    test *data = (test*)array->elts;
    printf("\n rps_array->nalloc==%lu\n",array->nalloc);
    printf("pool->current == %p,pool==%p\n",pool->current,pool);
    printf("pool->large==%p\n",pool->large);
    printf("pool->d.next == %p\n",pool->d.next);
    for (int i=0;i<40;i++)
     printf("%d ",data[i].num);

     rps_destroy_pool(pool);
}
