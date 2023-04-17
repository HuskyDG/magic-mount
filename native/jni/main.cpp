#include <sys/xattr.h>

#include "base.hpp"
#include "utils.hpp"


static int mount_flags = 0;
static bool verbose_logging = false;

#define verbose_log(...) { if (verbose_logging) fprintf(stderr, __VA_ARGS__); }

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

std::string parse_mode(int mode) {
    if (mode == 0)
        return "DIRECTORY";
    if (mode == 1)
        return "FILE";
    if (mode == 2)
        return "FIFO";
    if (mode == 3)
        return "SYMLINK";
    if (mode == 4)
        return "BLOCK";
    if (mode == 5)
        return "CHAR";
    return "WHITEOUT";
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

    void do_mount()
    {
        int mode = get_mode();

        switch (mode)
        {
        case 0:
        { // DIRECTORY
            mkdir(dest.data(), 0);
            clone_attr(src.data(), dest.data());
            verbose_log("magic_mount: mkdir %s <- %s\n", dest.data(), src.data());
            break;
        }
        case 1:
        case 2:
        { // FILE / FIFO
            close(open(dest.data(), O_RDWR | O_CREAT, 0755));
            mount(src.data(), dest.data(), nullptr, MS_BIND | mount_flags, nullptr);
            verbose_log("magic_mount: mnt_bind %s <- %s\n", dest.data(), src.data());
            break;
        }
        case 3:
        { // SYMLINK
            char buf[PATH_MAX];
            ssize_t n = readlink(src.data(), buf, sizeof(buf));
            if (n >= 0) {
                buf[n] = '\0';
                symlink(buf, dest.data());
                verbose_log("magic_mount: link %s <- %s\n", dest.data(), buf);
            }
            break;
        }
        case 4:
        { // BLOCK
            mknod(dest.data(), S_IFBLK, st.st_rdev);
            clone_attr(src.data(), dest.data());
            verbose_log("magic_mount: mknod block %s <- %s\n", dest.data(), src.data());
        }
        case 5:
        { // CHAR
            mknod(dest.data(), S_IFCHR, st.st_rdev);
            clone_attr(src.data(), dest.data());
            verbose_log("magic_mount: mknod char %s <- %s\n", dest.data(), src.data());
        }
        default:
        { // WHITEOUT
            // do nothing
            verbose_log("magic_mount: ignore %s <- %s\n", dest.data(), src.data());
            break;
        }
        }
    }
};

std::vector<item_node> item;

inline struct item_node *find_node_by_dest(const char *dest)
{
    for (auto it = item.begin(); it != item.end(); it++)
        if (it->dest == dest)
            return (struct item_node *)&(*it);
    return nullptr;
}

static void collect_mount(const char *src, const char *target)
{
    struct statfs stfs{};
    statfs(src, &stfs);
    if (stfs.f_type == PROC_SUPER_MAGIC) {
        return; // no magic mount /proc
        verbose_log("record: ignore src=[%s] unsupported fs\n", src);
    }
    auto s = find_node_by_dest(target);
    struct item_node m;
    m.src = src;
    m.dest = target;
    int mode = m.get_mode();
    if (s == nullptr) {
        item.emplace_back(m);
        verbose_log("record: src=[%s] type=[%s]\n", src, parse_mode(mode).data());
    }
    if (!S_ISDIR(m.st.st_mode) || (s && s->ignore))
        return;
    {
        char *trusted_opaque = new char[3];
        ssize_t ret = getxattr(src, "trusted.overlay.opaque", trusted_opaque, 3*sizeof(char));
        if (ret == 1 && trusted_opaque[0] == 'y') {
            verbose_log("record: src=[%s] marked as trusted opaque\n", src);
            item.back().ignore = true;
        }
        delete trusted_opaque;
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
            collect_mount(b1, b2);
        }
        closedir(dirfp);
    }
}

static void do_mount() {
    for (auto it = item.begin(); it != item.end(); it++)
        it->do_mount();
}

int main(int argc, const char **argv)
{
    const char *mnt_name = "tmpfs";

    first:
    if (argc < 3) {
        fprintf(stderr, "usage: %s [OPTION] DIR1 DIR2... DIR\n\n"
                        "Use magic mount to combine DIR1, DIR2... and mount into DIR\n\n"
                        "-r            Merge content of mounts under DIR1, DIR2... also\n"
                        "-n NAME       Give magic mount a nice name\n"
                        "-v            Verbose magic mount\n\n", basename(argv[0]));
        return 1;
    }
    if (strcmp(argv[argc-1], "/dev") == 0) {
        fprintf(stderr, "Invalid arguments\n");
        return -1;
    }

    if (argv[1][0] == '-') {
        if (strcmp(argv[1], "-r") == 0) {
            verbose_log("option: recursive\n");
            mount_flags |= MS_REC;
        } else if (strcmp(argv[1], "-n") == 0) {
            verbose_log("option: name=[%s]\n", argv[2]);
            mnt_name = argv[2];
            argc--; argv++;
        } else if (strcmp(argv[1], "-v") == 0) {
            verbose_logging = true;
        } else {
            fprintf(stderr, "Invalid options: [%s]\n", argv[1]);
            return 1;
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
        mount("tmpfs", tmp.data(), "tmpfs", 0, nullptr)) {
        verbose_log("error: unable to setup workdir=[%s]\n", tmp.data());
        goto failed;
    }

    for (int i=1; i < argc-1; i++) {
        struct statfs stfs{};
        statfs(argv[i], &stfs);
        if (stfs.f_type == PROC_SUPER_MAGIC) {
            fprintf(stderr, "Invalid arguments\n");
            goto failed;
         }
        char workdir[100];
        snprintf(workdir, sizeof(workdir), "%s/%d", tmp.data(), i);
        mkdir(workdir, 0755);
        if (mount(argv[i], workdir, nullptr, MS_BIND | mount_flags, nullptr)) {
            verbose_log("error: unable to setup layer[%d]=[%s]\n", i, argv[i]);
            goto failed;
        }
        verbose_log("setup: layer[%d]=[%s] workdir=[%s]\n", i, argv[i], workdir);
        mount("", workdir, nullptr, MS_PRIVATE | mount_flags, nullptr);
        collect_mount(workdir, argv[argc-1]);
    }
    if (item.empty()) {
        verbose_log("setup: nothing to magic mount\n");
        goto success;
    }
    verbose_log("setup: mountpoint=[%s]\n", argv[argc-1]);
    mount(mnt_name, argv[argc-1], "tmpfs", 0, nullptr);
    do_mount();
    mount(nullptr, argv[argc-1], nullptr, MS_REMOUNT | MS_RDONLY | MS_REC, nullptr);

    success:
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 0;
    
    failed:
    umount2(tmp.data(), MNT_DETACH);
    rmdir(tmp.data());
    return 1;
}
