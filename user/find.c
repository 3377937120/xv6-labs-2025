#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

static char*
basename_xv6(char *path)
{
  char *p = path + strlen(path);
  while(p > path && p[-1] != '/')
    p--;
  return p;
}

static void
run_exec(char *path, char **cmd, int ncmd)
{
  if(ncmd <= 0)
    return;
  if(ncmd + 1 >= MAXARG){
    fprintf(2, "find: too many -exec arguments\n");
    return;
  }

  char *args[MAXARG];
  for(int i = 0; i < ncmd; i++)
    args[i] = cmd[i];
  args[ncmd] = path;
  args[ncmd + 1] = 0;

  int pid = fork();
  if(pid < 0){
    fprintf(2, "find: fork failed\n");
    return;
  }
  if(pid == 0){
    exec(args[0], args);
    fprintf(2, "find: exec %s failed\n", args[0]);
    exit(1);
  }
  wait(0);
}

static void
find_walk(char *path, char *name, char **cmd, int ncmd)
{
  int fd;
  struct stat st;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if(st.type == T_FILE || st.type == T_DEVICE){
    if(strcmp(basename_xv6(path), name) == 0){
      if(ncmd)
        run_exec(path, cmd, ncmd);
      else
        printf("%s\n", path);
    }
    close(fd);
    return;
  }

  if(st.type != T_DIR){
    close(fd);
    return;
  }

  int plen = strlen(path);
  if(plen + 1 + DIRSIZ + 1 > 512){
    fprintf(2, "find: path too long: %s\n", path);
    close(fd);
    return;
  }

  char buf[512];
  strcpy(buf, path);
  char *p = buf + plen;
  if(plen == 0 || buf[plen - 1] != '/')
    *p++ = '/';

  struct dirent de;
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;

    char elem[DIRSIZ + 1];
    memmove(elem, de.name, DIRSIZ);
    elem[DIRSIZ] = 0;
    // xv6目录名可能以NUL填充，strcmp可直接使用。
    if(strcmp(elem, ".") == 0 || strcmp(elem, "..") == 0)
      continue;

    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;
    find_walk(buf, name, cmd, ncmd);
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "usage: find path name [-exec command args ...]\n");
    exit(1);
  }

  int exec_pos = 0;
  for(int i = 3; i < argc; i++){
    if(strcmp(argv[i], "-exec") == 0){
      exec_pos = i;
      break;
    }
  }

  char **cmd = 0;
  int ncmd = 0;
  if(exec_pos){
    if(exec_pos + 1 >= argc){
      fprintf(2, "find: missing command after -exec\n");
      exit(1);
    }
    cmd = &argv[exec_pos + 1];
    ncmd = argc - exec_pos - 1;
  }

  find_walk(argv[1], argv[2], cmd, ncmd);
  exit(0);
}