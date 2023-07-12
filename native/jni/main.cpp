#include <sys/xattr.h>
#include <errno.h>

#include "logging.hpp"
#include "base.hpp"
#include "utils.hpp"

int log_fd = -1;
static int mount_flags = 0;
static bool verbose_logging = false;
char **_argv;
int _argc;
bool full_magic_mount = false;

#define verbose_log(s, ...) { \
if (verbose_logging) fprintf(stdout, "%-12s: " s, __VA_ARGS__); \
LOGD("%-12s: " s, __VA_ARGS__); }

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
            verbose_log("%s <- %s\n", "mkdir", dest.data(), src.data());
            mkdir(dest.data(), 0);
            return clone_attr(src.data(), dest.data()) == 0;
            break;
        }
        case 1:
        case 2:
        { // FILE / FIFO
            verbose_log("%s <- %s\n", "bind_mnt", dest.data(), src.data());
            return close(open(dest.data(), O_RDWR | O_CREAT, 0755)) == 0 &&
                mount(src.data(), dest.data(), nullptr, MS_BIND | mount_flags, nullptr) == 0;
            break;
        }
        case 3:
        { // SYMLINK
            char buf[PATH_MAX];
            ssize_t n = readlink(src.data(), buf, sizeof(buf));
            verbose_log("%s <- %s\n", "symlink", dest.data(), src.data());
            if (n >= 0) {
                buf[n] = '\0';
                return symlink(buf, dest.data()) == 0;
            }
            return false;
            break;
        }
        case 4:
        { // BLOCK
            verbose_log("%s <- %s\n", "mknod_chr", dest.data(), src.data());
            return mknod(dest.data(), S_IFBLK, st.st_rdev) == 0 &&
                clone_attr(src.data(), dest.data()) == 0;
        }
        case 5:
        { // CHAR
            verbose_log("%s <- %s\n", "mknod_blk", dest.data(), src.data());
            return mknod(dest.data(), S_IFCHR, st.st_rdev) == 0 &&
                clone_attr(src.data(), dest.data()) == 0;
        }
        default:
        { // WHITEOUT
            // do nothing
            verbose_log("%s <- %s\n", "ignore", dest.data(), src.data());
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

static bool magic_mount(const char *src, const char *target, int layer_number)
{
    if (!is_supported_fs(src)) {
        verbose_log("ignore src=[%s] unsupported fs\n", "magic_mount", src);
        return true; // no magic mount /proc
    }
    {
        struct stat st;
        auto s = find_node_by_dest(target);
        struct item_node m;
        m.src = src;
        m.dest = target;
        int mode = m.get_mode();
        bool first = false;
        if (s == nullptr) {
            item.emplace_back(m);
            if (!m.do_mount())
                return false;
            s = &(item.back());
            first = true && !full_magic_mount;
        }
        if (s && (s->ignore || // trusted opaque
                   s->get_mode() != 0 /* mounted (upper) node is regular file */))
            return true;
        if (!S_ISDIR(m.st.st_mode) || // regular file
            (lstat(src, &st) == 0 && !S_ISDIR(st.st_mode))) {
            s->ignore = true;
            return true;
        }
        {
            char *trusted_opaque = new char[3];
            ssize_t ret = getxattr(src, "trusted.overlay.opaque", trusted_opaque, 3*sizeof(char));
            if (ret == 1 && trusted_opaque[0] == 'y') {
                verbose_log("%s marked as trusted opaque\n", "magic_mount", target);
                s->ignore = true;
                if (first) return mount(src, target, nullptr, MS_BIND | mount_flags, nullptr) == 0;
            }
            delete[] trusted_opaque;
        }
        if (first) {
        // test if this position does not exist in lower layer
            const char *_root = strstr(src, "/");
            if (_root == nullptr) _root = "";
            bool last = true;
            for (int i = layer_number + 1; i < _argc -1; i++) {
                std::string layerdir = std::to_string(i) + _root;
                if (lstat(layerdir.data(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    // there is folder in lower layer...
                    last = false;
                    break;
                }
            }
            if (last) {
                // marked as unmerged folder to reduce wasting magic mount
                verbose_log("%s marked as unmerged folder\n", "magic_mount", target);
                s->ignore = true;
                return mount(src, target, nullptr, MS_BIND | mount_flags, nullptr) == 0;
            }
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
            if (!magic_mount(b1, b2, layer_number))
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
    const char *real_dir = nullptr;

    first:
    if (argc < 3) {
        fprintf(stderr, "usage: %s [OPTION] DIR1 DIR2... DIR\n\n"
                        "Use magic mount to combine DIR1, DIR2... and mount into DIR\n\n"
                        "-r            Recursive magic mount mountpoint under DIR1, DIR2... also\n"
                        "-n NAME       Give magic mount a nice name\n"
                        "-v [-/FILE]   Verbose magic mount to stdout [-] or file\n"
                        "-a            Always use magic mount for any case\n"
                        "-o [MNTFLAGS] Mount flags\n"
                        "\n", basename(argv[0]));
        return 1;
    }

    if (argv[1][0] == '-') {
        char *argv_option = argv[1];
        for (int i = 1; argv_option[i] != '\0'; ++i) {
            if (argv_option[i] == 'r') {
                verbose_log("recursive\n", "option");
                mount_flags |= MS_REC;
            } else if (argv_option[i] == 'n' && argv_option[i+1] == '\0') {
                verbose_log("name=[%s]\n", "option", argv[2]);
                mnt_name = argv[2];
                argc--; argv++;
                break;
            } else if (argv_option[i] == 'v' && argv_option[i+1] == '\0') {
                if (strcmp(argv[2], "-") == 0) {
                    verbose_logging = true;
                } else {
                    if (log_fd >= 0)
                        break;
                    verbose_log("log to file=[%s]\n", "option", argv[2]);
                    log_fd = open(argv[2], O_RDWR | O_CREAT | O_APPEND, 0666);
                }
                argc--; argv++;
                break;
            } else if (argv_option[i] == 'o' && argv_option[i+1] == '\0') {
                std::string mnt_opts = argv[2];
                auto opts = split_ro(mnt_opts, ',');
                for (auto &s : opts) {
                        if (s == "nosuid") {
                            mount_flags |= MS_NOSUID;
                        } else if (s == "lazytime") {
                            mount_flags |= MS_LAZYTIME;
                        } else if (s == "nodev") {
                            mount_flags |= MS_NODEV;
                        } else if (s == "noexec") {
                            mount_flags |= MS_NOEXEC;
                        } else if (s == "sync") {
                            mount_flags |= MS_SYNCHRONOUS;
                        } else if (s == "dirsync") {
                            mount_flags |= MS_DIRSYNC;
                        } else if (s == "noatime") {
                            mount_flags |= MS_NOATIME;
                        } else if (s == "nodiratime") {
                            mount_flags |= MS_NODIRATIME;
                        } else if (s == "relatime") {
                            mount_flags |= MS_RELATIME;
                        } else if (s == "strictatime") {
                            mount_flags |= MS_STRICTATIME;
                        } else if (s == "lazytime") {
                            mount_flags |= MS_LAZYTIME;
                        } else if (s == "nosymfollow") {
                            mount_flags |= MS_NOSYMFOLLOW;
                        } else if (s == "mand") {
                            mount_flags |= MS_MANDLOCK;
                        } else if (s == "silent") {
                            mount_flags |= MS_SILENT;
                        }
                }
                argc--; argv++;
                break;
            } else if (argv_option[i] == 'a') {
                full_magic_mount = true;
            } else {
                fprintf(stderr, "Invalid options: [%s]\n", argv[1]);
                return 1;
            }
        }
        argc--; argv++;
        goto first;
    }

    if (strcmp(argv[argc-1], "/dev") == 0 || !is_dir(argv[argc-1], true) || (real_dir = realpath(argv[argc-1], nullptr)) == nullptr) {
        fprintf(stderr, "mount: '%s'->'%s': %s\n", mnt_name, argv[argc-1], reason);
        return -1;
    } 

    std::string tmp;
    do {
        tmp = "/dev/.workdir_";
        tmp += random_strc(20);
    } while (access(tmp.data(), F_OK) == 0);
    verbose_log("workdir=[%s]\n", "setup", tmp.data());
    if (mkdir(tmp.data(), 0755) ||
        mount("tmpfs", tmp.data(), "tmpfs", 0, nullptr) ||
        chdir(tmp.data())) {
        verbose_log("unable to setup workdir=[%s]\n", "error", tmp.data());
        reason = "Unable to create working directory";
        goto failed;
    }

    for (int i=1; i < argc-1; i++) {
        if (!is_supported_fs(argv[i])) {
            goto failed;
         }
    }
    // setup workdir first
    _argv = argv;
    _argc = argc;
    {
        mkdir("0", 0755);
        for (int i=1; i < argc-1; i++) {
            char workdir[12];
            snprintf(workdir, sizeof(workdir), "%d", i);
            mkdir(workdir, 0755);
            verbose_log("layerdir[%d]=[%s]\n", "setup", i, argv[i]);
            if (!mount(argv[i], workdir, nullptr, MS_BIND | mount_flags, nullptr) &&
                !mount("", workdir, nullptr, MS_PRIVATE | mount_flags, nullptr)) {
                continue;
            }
            verbose_log("setup failed\n", "magic_mount");
            reason = std::strerror(errno);
            goto failed;
        }
        verbose_log("magic mount layerdir[0]=[%s]\n", "setup", real_dir);
        if (mount(mnt_name, "0", "tmpfs", 0, nullptr)) {
            reason = std::strerror(errno);
            goto failed;
        }
        for (int i=1; i < argc-1; i++) {
            if (magic_mount(std::to_string(i).data(), "0", i)) {
                continue;
            }
            verbose_log("mount failed\n", "magic_mount");
            goto failed;
        }
    }

    // remount to read-only
    mount(nullptr, "0", nullptr, MS_REMOUNT | MS_RDONLY | MS_REC | mount_flags, nullptr);
    // make mount as private so we can move mounts
    mount(nullptr, "0", nullptr, MS_PRIVATE | MS_REC, nullptr);
    if (mount("0", real_dir, nullptr, MS_MOVE, nullptr) == -1 &&
        // recursive bind mount if moving mount does not work
        mount("0", real_dir, nullptr, MS_BIND | MS_REC, nullptr) == -1) {
        reason = std::strerror(errno);
        goto failed;
    }
    verbose_log("mounted to %s\n", "magic_mount", real_dir);

    success:
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 0;
    
    failed:
    fprintf(stderr, "mount: '%s'->'%s': %s\n", mnt_name, real_dir, reason);
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 1;
}
