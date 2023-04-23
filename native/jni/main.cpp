#include <sys/xattr.h>
#include <errno.h>

#include "logging.hpp"
#include "base.hpp"
#include "utils.hpp"

int log_fd = -1;
bool enable_logging = false;
static int mount_flags = 0;
static bool verbose_logging = false;

#define verbose_log(...) { \
if (verbose_logging) fprintf(stderr, __VA_ARGS__); \
LOGD(__VA_ARGS__); }

static bool is_supported_fs(const char *dir) {
    struct statfs st;
    if (statfs(dir, &st) == 0) {
       switch (st.f_type) {
           case PROC_SUPER_MAGIC:
           case SELINUX_MAGIC:
           case SYSFS_MAGIC:
               return false;
       }
       return true;
    }
    return false;
}

int clone_attr(const char *src, const char *dest) 
{
    struct stat st;
    char *con;
    if (stat(src, &st) || getfilecon(src, &con) == -1)
        return -1;
    bool ret = (chmod(dest, (st.st_mode & 0777)) ||
        chown(dest, st.st_uid, st.st_gid) ||
        setfilecon(dest, con));
    freecon(con);
    return (ret)? -1 : 0;
}

struct item_node
{
    std::string src;
    std::string dest;
    struct stat st;
    bool ignore = false;

    int get_mode()
    {
        lstat(src.data(), &st);
        if (S_ISDIR(st.st_mode))
            return 0;
        if (S_ISREG(st.st_mode))
            return 1;
        if (S_ISFIFO(st.st_mode))
            return 2;
        if (S_ISLNK(st.st_mode))
            return 3;
        if (S_ISBLK(st.st_mode))
            return 4;
        if (S_ISCHR(st.st_mode) && st.st_rdev > 0)
            return 5;
        return -1;
    }

    bool do_mount()
    {
        int mode = get_mode();

        switch (mode)
        {
        case 0:
        { // DIRECTORY
            verbose_log("%-10s: %s <- %s\n", "mkdir", dest.data(), src.data());
            mkdir(dest.data(), 0);
            return clone_attr(src.data(), dest.data()) == 0;
            break;
        }
        case 1:
        case 2:
        { // FILE / FIFO
            verbose_log("%-10s: %s <- %s\n", "bind_mnt", dest.data(), src.data());
            return close(open(dest.data(), O_RDWR | O_CREAT, 0755)) == 0 &&
                mount(src.data(), dest.data(), nullptr, MS_BIND | mount_flags, nullptr) == 0;
            break;
        }
        case 3:
        { // SYMLINK
            char buf[PATH_MAX];
            ssize_t n = readlink(src.data(), buf, sizeof(buf));
            verbose_log("%-10s: %s <- %s\n", "symlink", dest.data(), src.data());
            if (n >= 0) {
                buf[n] = '\0';
                return symlink(buf, dest.data()) == 0;
            }
            return false;
            break;
        }
        case 4:
        { // BLOCK
            verbose_log("%-10s: %s <- %s\n", "mknod_chr", dest.data(), src.data());
            return mknod(dest.data(), S_IFBLK, st.st_rdev) == 0 &&
                clone_attr(src.data(), dest.data()) == 0;
        }
        case 5:
        { // CHAR
            verbose_log("%-10s: %s <- %s\n", "mknod_blk", dest.data(), src.data());
            return mknod(dest.data(), S_IFCHR, st.st_rdev) == 0 &&
                clone_attr(src.data(), dest.data()) == 0;
        }
        default:
        { // WHITEOUT
            // do nothing
            verbose_log("%-10s: %s <- %s\n", "ignore", dest.data(), src.data());
            return true;
            break;
        }
        }
    }
};

std::vector<item_node> item;

inline struct item_node *find_node_by_dest(std::string_view dest)
{
    for (auto it = item.begin(); it != item.end(); it++)
        if (std::string_view(it->dest) == dest)
            return (struct item_node *)&(*it);
    return nullptr;
}

static bool magic_mount(const char *src, const char *target)
{
    if (!is_supported_fs(src)) {
        verbose_log("magic_mount: ignore src=[%s] unsupported fs\n", src);
        return true; // no magic mount /proc
    }
    {
        auto s = find_node_by_dest(target);
        struct item_node m;
        m.src = src;
        m.dest = target;
        int mode = m.get_mode();
        if (s == nullptr) {
            item.emplace_back(m);
            if (!m.do_mount())
                return false;
            s = &(item.back());
        }
        if (!S_ISDIR(m.st.st_mode) || // regular file
            (s && (s->ignore || // trusted opaque
                   s->get_mode() != 0 /* mounted (upper) node is regular file */)))
            return true;
        {
            char *trusted_opaque = new char[3];
            ssize_t ret = getxattr(src, "trusted.overlay.opaque", trusted_opaque, 3*sizeof(char));
            if (ret == 1 && trusted_opaque[0] == 'y') {
                verbose_log("magic_mount: %s marked as trusted opaque\n", target);
                s->ignore = true;
            }
            delete[] trusted_opaque;
        }
    }
    struct dirent *dp;
    DIR *dirfp = opendir(src);
    if (dirfp != nullptr)
    {
        while ((dp = readdir(dirfp)) != nullptr)
        {
            if (strcmp(dp->d_name, ".") == 0 ||
                strcmp(dp->d_name, "..") == 0)
                continue;
            char b1[PATH_MAX], b2[PATH_MAX];
            snprintf(b1, sizeof(b1), "%s/%s", src, dp->d_name);
            snprintf(b2, sizeof(b2), "%s/%s", target, dp->d_name);
            if (!magic_mount(b1, b2))
                return false;
        }
        closedir(dirfp);
        return true;
    }
    return false;
}
int main(int argc, char **argv)
{
    const char *mnt_name = "tmpfs";
    const char *reason = "Invalid arguments";

    first:
    if (argc < 3) {
        fprintf(stderr, "usage: %s [OPTION] DIR1 DIR2... DIR\n\n"
                        "Use magic mount to combine DIR1, DIR2... and mount into DIR\n\n"
                        "-r            Merge content of mounts under DIR1, DIR2... also\n"
                        "-n NAME       Give magic mount a nice name\n"
                        "-v            Verbose magic mount to stderr\n"
                        "-l            Verbose magic mount to logd\n"
                        "-f FILE       Verbose magic mount to file\n"
                        "\n", basename(argv[0]));
        return 1;
    }
    if (strcmp(argv[argc-1], "/dev") == 0 || !is_dir(argv[argc-1])) {
        fprintf(stderr, "mount: '%s'->'%s': %s\n", mnt_name, argv[argc-1], reason);
        return -1;
    }

    if (argv[1][0] == '-') {
        char *argv_option = argv[1];
        for (int i = 1; argv_option[i] != '\0'; ++i) {
            if (argv_option[i] == 'r') {
                verbose_log("option: recursive\n");
                mount_flags |= MS_REC;
            } else if (argv_option[i] == 'n' && argv_option[i+1] == '\0') {
                verbose_log("option: name=[%s]\n", argv[2]);
                mnt_name = argv[2];
                argc--; argv++;
                break;
            } else if (argv_option[i] == 'l') {
                enable_logging = true;
            } else if (argv_option[i] == 'f' && argv_option[i+1] == '\0') {
                if (log_fd >= 0)
                    break; 
                verbose_log("option: log to file=[%s]\n", argv[2]);
                log_fd = open(argv[2], O_RDWR | O_CREAT | O_APPEND, 0666);
                argc--; argv++;
                break;
            } else if (argv_option[i] == 'v') {
                verbose_logging = true;
            } else {
                fprintf(stderr, "Invalid options: [%s]\n", argv[1]);
                return 1;
            }
        }
        argc--; argv++;
        goto first;
    }

    std::string tmp;
    do {
        tmp = "/dev/.workdir_";
        tmp += random_strc(20);
    } while (access(tmp.data(), F_OK) == 0);
    verbose_log("setup: workdir=[%s]\n", tmp.data());
    if (mkdir(tmp.data(), 0755) ||
        mount("tmpfs", tmp.data(), "tmpfs", 0, nullptr) ||
        chdir(tmp.data())) {
        verbose_log("error: unable to setup workdir=[%s]\n", tmp.data());
        reason = "Unable to create working directory";
        goto failed;
    }

    for (int i=1; i < argc-1; i++) {
        if (!is_supported_fs(argv[i])) {
            goto failed;
         }
    }
    // setup workdir first
    {
        std::vector<std::string> workdirs;
        for (int i=1; i < argc-1; i++) {
            char workdir[12];
            snprintf(workdir, sizeof(workdir), "%d", i);
            mkdir(workdir, 0755);
            verbose_log("setup: layerdir[%d]=[%s]\n", i, argv[i]);
            if (!mount(argv[i], workdir, nullptr, MS_BIND | mount_flags, nullptr) &&
                !mount("", workdir, nullptr, MS_PRIVATE | mount_flags, nullptr)) {
                   workdirs.push_back(workdir);
                continue;
            }
            verbose_log("magic_mount: setup failed\n");
            reason = std::strerror(errno);
            goto failed;
        }
        verbose_log("setup: mountpoint=[%s]\n", argv[argc-1]);
        if (mount(mnt_name, argv[argc-1], "tmpfs", 0, nullptr)) {
            reason = std::strerror(errno);
            goto failed;
        }
        struct stat st_mnt{}, st_unmnt{};
        stat(argv[argc-1], &st_mnt);
        for (auto &m : workdirs) {
            if (magic_mount(m.data(), argv[argc-1])) {
                continue;
            }
            verbose_log("magic_mount: mount failed\n");
            if (stat(argv[argc-1], &st_unmnt) == 0 &&
                st_mnt.st_dev == st_unmnt.st_dev &&
                st_mnt.st_ino == st_unmnt.st_ino)
                // don't unmount if tmpfs is unmounted by another one
                umount2(argv[argc-1], MNT_DETACH);
            goto failed;
        }
    }

    // remount to read-only
    mount(nullptr, argv[argc-1], nullptr, MS_REMOUNT | MS_RDONLY | MS_REC, nullptr);

    success:
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 0;
    
    failed:
    fprintf(stderr, "mount: '%s'->'%s': %s\n", mnt_name, argv[argc-1], reason);
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 1;
}
