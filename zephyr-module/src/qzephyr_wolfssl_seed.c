#include <zephyr/random/random.h>

int qzephyr_wolfssl_seed(unsigned char *output, unsigned int sz)
{
    sys_rand_get(output, sz);
    return 0;
}
