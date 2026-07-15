#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static const char *separators = " -\r\t\n./,";

static int
is_separator(char c)
{
  return strchr(separators, c) != 0;
}

static void
emit_if_needed(int value, int have_digits, int valid_token)
{
  if(have_digits && valid_token && (value % 5 == 0 || value % 6 == 0))
    printf("%d\n", value);
}

static void
scan_fd(int fd)
{
  char c;
  int value = 0;
  int have_digits = 0;
  int valid_token = 1;
  int at_boundary = 1; // 文件开头视为分隔符。

  while(read(fd, &c, 1) == 1){
    if(c >= '0' && c <= '9'){
      // 只有在文件开头或合法分隔符之后，数字才可开始一个数。
      if(!have_digits && !at_boundary)
        valid_token = 0;
      have_digits = 1;
      value = value * 10 + (c - '0');
      at_boundary = 0;
    } else if(is_separator(c)){
      emit_if_needed(value, have_digits, valid_token);
      value = 0;
      have_digits = 0;
      valid_token = 1;
      at_boundary = 1;
    } else {
      // “xv6”中的6不能算数字；遇到普通字符后，本 token 作废。
      valid_token = 0;
      at_boundary = 0;
    }
  }

  // 文件结尾是隐式分隔符。
  emit_if_needed(value, have_digits, valid_token);
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: sixfive file ...\n");
    exit(1);
  }

  for(int i = 1; i < argc; i++){
    int fd = open(argv[i], O_RDONLY);
    if(fd < 0){
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      continue;
    }
    scan_fd(fd);
    close(fd);
  }

  exit(0);
}
