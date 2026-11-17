#include <stdint.h>
#include <stdarg.h>
#include <zephyr/sys/byteorder.h>

extern int zvfs_ioctl(int fd, unsigned long request, va_list args);

uint32_t htonl(uint32_t hostlong)
{
    return sys_cpu_to_be32(hostlong);
}

uint16_t htons(uint16_t hostshort)
{
    return sys_cpu_to_be16(hostshort);
}

uint32_t ntohl(uint32_t netlong)
{
    return sys_be32_to_cpu(netlong);
}

uint16_t ntohs(uint16_t netshort)
{
    return sys_be16_to_cpu(netshort);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    const int ret = zvfs_ioctl(fd, request, ap);
    va_end(ap);
    return ret;
}
