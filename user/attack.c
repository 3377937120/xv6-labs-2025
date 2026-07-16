#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define DATASIZE (8 * PGSIZE)

static int
is_alnum(char c)
{
  return (c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z');
}

int
main(int argc, char *argv[])
{
  // Your code here.
  (void)argc;
  (void)argv;

  char *mem;
  const char *hint = "This may help.";

  // 申请与 secret.c 中 data 数组相同大小的内存。
  mem = sbrk(DATASIZE);

  if(mem == (char *)-1)
    exit(1);

  // 在刚刚分配的内存中逐字节寻找 secret.c 留下的提示字符串。
  for(int i = 0; i + 16 < DATASIZE; i++){
    int j;

    // 比较当前位置是否出现 "This may help."
    for(j = 0; hint[j] != '\0'; j++){
      if(mem[i + j] != hint[j])
        break;
    }

    // 必须完整匹配提示字符串，并且后面是字符串结束符。
    if(hint[j] != '\0' || mem[i + j] != '\0')
      continue;

    // secret.c 将真正的secret写在提示字符串起点之后的第16字节。
    char *secret = mem + i + 16;
    int len = 0;

    while(i + 16 + len < DATASIZE &&
          is_alnum(secret[len])){
      len++;
    }

    // 要求找到非空字符串，并且字符串在已分配范围内以 '\0' 结束。
    if(len > 0 &&
       i + 16 + len < DATASIZE &&
       secret[len] == '\0'){

      write(1, secret, len);
      write(1, "\n", 1);

      exit(0);
    }
  }

  // 本次没有重新获得存放secret的物理页。
  exit(1);
}
