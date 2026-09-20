#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

void find(char *path, char *target){
    int fd;
    struct stat st;
    struct dirent de;
    char buf[512];
    char *p;

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    char *name = path + strlen(path);

    while (name > path && *(name - 1) != '/'){
        name --;
    }

    if (strcmp(name, target) == 0) {
        printf("%s\n", path);
    }

    switch (st.type) {
        case T_FILE:

            break;

        case T_DIR:
            if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
                fprintf(2, "find: path too long\n");
                break;
            }
            
            strcpy(buf, path);

            p = buf + strlen(buf);
            *p++ = '/';

            while (read(fd, &de, sizeof(de)) == sizeof(de)){
                if (de.inum == 0) {
                    continue;
                }

                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = '\0';

                if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
                    continue;
                }

                find(buf, target);
            }

            break;
    }

    close(fd);
}

int main(int argc, char *argv[]){
    if (argc != 3) {
        fprintf(2, "usage: find <path> <target>\n");
        exit(1);
    }

    find(argv[1], argv[2]);
    exit(0);
}