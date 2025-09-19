#include <iostream>
#include <cstring>
#include <regex>

#include "util/util.hh"
#include "util/logging.hh"
#include "main/shared.hh"
#include "store/filetransfer.hh"
#include "util/archive.hh"
#include "util/compression.hh"
#include "store/nar-accessor.hh"
#include "util/sync.hh"
#include "util/environment-variables.hh"
#include "util/users.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#define FUSE_USE_VERSION 30
#include <fuse.h>

#include <nlohmann/json.hpp>

using namespace nix;

typedef std::vector<std::string> PathSeq;

PathSeq readmePath{"README"};

static std::string readmeText =
R"str(This is a virtual file system that automatically fetches debug info
files when requested via .build-id/<build-id>.debug. For more
information, see https://github.com/edolstra/dwarffs.
)str";

PathSeq buildidPath{".build-id"};

std::regex debugFileRegex("^[0-9a-f]{38}\\.debug$");

std::vector<std::string> debugInfoServers{"https://cache.nixos.org/debuginfo"};

/* How long to remember negative lookups. */
unsigned int negativeTTL = 24 * 60 * 60;

Path cacheDir;

struct DebugFile
{
    const Path path;
    const size_t size;
    Sync<AutoCloseFD> fd;
    DebugFile(const Path & path, size_t size)
        : path(path), size(size)
    { }
};

static Sync<std::map<std::string, std::shared_ptr<DebugFile>>> files_;

static uid_t uid = (uid_t) -1;
static gid_t gid = (gid_t) -1;

/* Return true iff q is inside p. */
bool isInside(const PathSeq & q, const PathSeq & p)
{
    auto i = p.begin();
    auto j = q.begin();
    while (true) {
        if (i == p.end()) return true;
        if (j == q.end()) return false;
        if (*i != *j) return false;
        ++i;
        ++j;
    }
}

bool isInsideBuildid(const PathSeq & path)
{
    if (path.size() <= buildidPath.size()
        || !isInside(path, buildidPath))
        return false;
    auto & name = path[buildidPath.size()];
    return name.size() == 2 && isxdigit(name[0]) && isxdigit(name[1]);
}

bool isDebugFile(const PathSeq & path)
{
    if (!isInsideBuildid(path) || path.size() != buildidPath.size() + 2)
        return false;
    auto & name = path[buildidPath.size() + 1];
    return std::regex_match(name, debugFileRegex);
}

std::string toBuildId(const PathSeq & path)
{
    assert(path[buildidPath.size()].size() == 2);
    assert(path[buildidPath.size() + 1].size() == 44);
    return path[buildidPath.size()] + std::string(path[buildidPath.size() + 1], 0, 38);
}

std::string canonUri(const std::string & uri)
{
    auto i2 = uri.find("://");
    if (i2 == std::string::npos)
        throw Error("'%s' is not a URI", uri);

    i2 += 3;
    std::string s(uri, 0, i2);

    std::string::const_iterator i = uri.begin() + i2, end = uri.end();
    std::string temp;

    bool first = true;

    while (1) {

        /* Skip slashes. */
        while (i != end && *i == '/') i++;
        if (i == end) break;

        /* Ignore `.'. */
        if (*i == '.' && (i + 1 == end || i[1] == '/'))
            i++;

        /* If `..', delete the last component. */
        else if (*i == '.' && i + 1 < end && i[1] == '.' &&
            (i + 2 == end || i[2] == '/'))
        {
            if (!s.empty()) s.erase(s.rfind('/'));
            i += 2;
        }

        /* Normal component; copy it. */
        else {
            if (!first) s += '/';
            first = false;
            while (i != end && *i != '/') s += *i++;
        }
    }

    return s;
}

std::shared_ptr<DebugFile> haveDebugFileUncached(const std::string & buildId, bool download)
{
    auto path = cacheDir + "/" + buildId;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) return nullptr;

        if (st.st_size != 0) {
            debug("got cached '%s'", path);
            return std::make_shared<DebugFile>(path, st.st_size);
        } else if (st.st_mtime > time(0) - negativeTTL) {
            debug("got negative cached '%s'", path);
            return nullptr;
        }

    } else if (errno != ENOENT)
          return nullptr;

    std::function<std::shared_ptr<DebugFile>(std::string)> tryUri;

    tryUri = [&](std::string uri) {
        FileTransferRequest req(canonUri(uri));

        try {

            auto res = getFileTransfer()->download(req);

            /* Decompress .xz files. */
            if (std::string(res.data, 0, 5) == "\xfd" "7zXZ") {
                debug("'%s' return xz data", uri);
                res.data = decompress("xz", std::move(res.data));
            }

            /* If this is an ELF file, assume it's the raw debug info
               file. */
            if (std::string(res.data, 0, 4) == "\x7f" "ELF") {
                debug("'%s' returned ELF debug info file for '%s'", uri, buildId);
                writeFile(path, std::move(res.data));
                return std::make_shared<DebugFile>(path, res.data.size());
            }

            /* If this is a JSON file, assume it's a redirect
               file. This is used in cache.nixos.org to redirect to
               the NAR file containing the debug info files for a
               particular store path. */
            else if (std::string(res.data, 0, 1) == "{") {
                debug("'%s' returned JSON redirection", uri);

                auto json = nlohmann::json::parse(std::move(res.data));

                // FIXME
                std::string archive = json["archive"];
                auto uri2 = dirOf(uri) + "/" + archive;
                return tryUri(uri2);
            }

            /* If this is a NAR file, extract all debug info file,
               not just the one we need right now. After all, disk
               space is cheap but latency isn't. */
            else if (hasPrefix(res.data, std::string("\x0d\x00\x00\x00\x00\x00\x00\x00", 8) + narVersionMagic1)) {
                debug("'%s' returned a NAR", uri);

                auto accessor = makeNarAccessor(std::move(res.data));

                std::function<void(const CanonPath &)> doPath;

                std::regex debugFileRegex("^/lib/debug/\\.build-id/[0-9a-f]{2}/[0-9a-f]{38}\\.debug$");
                std::string debugFilePrefix = "/lib/debug/.build-id/";

                doPath = [&](const CanonPath & curPath) {
                    auto st = accessor->lstat(curPath);

                    if (st.type == SourceAccessor::Type::tDirectory) {
                        for (auto & [name, type] : accessor->readDirectory(curPath))
                            doPath(curPath / name);
                    }

                    else if (st.type == SourceAccessor::Type::tRegular && std::regex_match(curPath.abs(), debugFileRegex)) {
                        std::string buildId2 =
                            curPath.abs().substr(debugFilePrefix.size(), 2) +
                            curPath.abs().substr(debugFilePrefix.size() + 3, 38);

                        debug("got ELF debug info file for %s from NAR at '%s'", buildId2, uri);

                        writeFile(cacheDir + "/" + buildId2, accessor->readFile(curPath));
                    }
                };

                doPath(CanonPath::root);

                /* Check if we actually got the debug info file we
                   want. */
                return haveDebugFileUncached(buildId, false);
            }

            printError("got unsupported data from '%s'", uri);
            return std::shared_ptr<DebugFile>();

        } catch (FileTransferError & e) {
            if (e.error != FileTransfer::NotFound)
                printError("while downloading '%s': %s", uri, e.what());
        }

        return std::shared_ptr<DebugFile>();
    };

    if (download) {
        for (auto & server : debugInfoServers) {
            printInfo("fetching '%s' from '%s'...", buildId, server);
            auto file = tryUri(server + "/" + buildId);
            if (file) return file;
        }
    }

    /* Write an empty marker to cache negative lookups. */
    writeFile(path, "");

    return nullptr;
}

std::shared_ptr<DebugFile> haveDebugFile(const std::string & buildId)
{
    {
        auto files(files_.lock());
        auto i = files->find(buildId);

        if (i != files->end())
            // FIXME: respect TTL
            return i->second;
    }

    auto file = haveDebugFileUncached(buildId, true);

    {
        auto files(files_.lock());
        auto i = files->find(buildId);
        if (i != files->end()) return i->second;
        (*files)[buildId] = file;
    }

    return file;
}

static int dwarffs_getattr(const char * path_, struct stat * stbuf)
{
    try {

        int res = 0;

        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_uid = uid;
        stbuf->st_gid = gid;

        auto path = tokenizeString<PathSeq>(path_, "/");

        if (isInside(buildidPath, path)
            || (isInsideBuildid(path) && path.size() == buildidPath.size() + 1))
        {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
        }
        else if (isDebugFile(path)) {
            auto buildId = toBuildId(path);
            auto file = haveDebugFile(buildId);
            if (file) {
                stbuf->st_mode = S_IFREG | 0555;
                stbuf->st_nlink = 1;
                stbuf->st_size = file->size;
            } else
                res = -ENOENT;
        }
        else if (path == readmePath) {
            stbuf->st_mode = S_IFREG | 0444;
            stbuf->st_nlink = 1;
            stbuf->st_size = readmeText.size();
        }
        else
            res = -ENOENT;

        return res;

    } catch (std::exception & e) {
        ignoreExceptionExceptInterrupt();
        return -EIO;
    }
}

static int dwarffs_readdir(const char * path_, void * buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info * fi)
{
    auto path = tokenizeString<PathSeq>(path_, "/");

    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    if (path == PathSeq{}) {
        filler(buf, "README", nullptr, 0);
        filler(buf, ".build-id", nullptr, 0);
    }
    else if (path == buildidPath) {
        static std::string hexDigits = "0123456789abcdef";
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++) {
                char fn[3] = "00";
                fn[0] = hexDigits[i];
                fn[1] = hexDigits[j];
                filler(buf, fn, nullptr, 0);
            }
    }
    else
        return -ENOENT;

    return 0;
}

static int dwarffs_open(const char * path_, struct fuse_file_info * fi)
{
    try {

        auto path = tokenizeString<PathSeq>(path_, "/");

        if (path != readmePath
            && !(isDebugFile(path) && haveDebugFile(toBuildId(path))))
            return -ENOENT;

        if ((fi->flags & 3) != O_RDONLY)
            return -EACCES;

        return 0;

    } catch (std::exception & e) {
        ignoreExceptionExceptInterrupt();
        return -EIO;
    }
}

static int dwarffs_read(const char * path_, char * buf, size_t size, off_t offset,
    struct fuse_file_info * fi)
{
    try {

        auto path = tokenizeString<PathSeq>(path_, "/");

        std::shared_ptr<DebugFile> file;

        if (path == readmePath) {
            auto len = readmeText.size();
            if (offset < (off_t) len) {
                if (offset + size > len)
                    size = len - offset;
                memcpy(buf, readmeText.data() + offset, size);
                return size;
            } else
                return 0;
        }

        else if (isDebugFile(path) && (file = haveDebugFile(toBuildId(path)))) {

            auto fd(file->fd.lock());

            if (fd->get() == -1) {
                debug("opening '%s'", file->path);
                *fd = open(file->path.c_str(), O_RDONLY);
                if (fd->get() == -1) return -EIO;
            }

            return pread(fd->get(), buf, size, offset);
        }

        else
            return -ENOENT;

    } catch (std::exception & e) {
        ignoreExceptionExceptInterrupt();
        return -EIO;
    }
}

struct dwarffs_param
{
    char * cache = nullptr;
    char * uid = nullptr;
    char * gid = nullptr;
};

enum { KEY_HELP, KEY_VERSION };

#define DWARFFS_OPT(t, p) { t, offsetof(struct dwarffs_param, p), 1 }

static const struct fuse_opt dwarffs_opts[] = {
        DWARFFS_OPT("cache=%s", cache),
        DWARFFS_OPT("uid=%s", uid),
        DWARFFS_OPT("gid=%s", gid),
        FUSE_OPT_KEY("--version", KEY_VERSION),
        FUSE_OPT_KEY("--help", KEY_HELP),
        FUSE_OPT_END
};

static fuse_operations oper;

static int dwarffs_opt_proc(void * data, const char * arg, int key, struct fuse_args * outargs)
{
     switch (key) {
     case KEY_HELP:
         fuse_opt_add_arg(outargs, "-h");
         fuse_main(outargs->argc, outargs->argv, &oper, nullptr);
         exit(0);
     case KEY_VERSION:
         printError("dwarffs version: %s", VERSION);
         fuse_opt_add_arg(outargs, "--version");
         fuse_main(outargs->argc, outargs->argv, &oper, nullptr);
         exit(0);
     }
     return 1;
}

static void mainWrapped(int argc, char * * argv)
{
    verbosity = lvlDebug;

    /* Handle being invoked by mount with a "fuse.dwarffs" mount
       type. */
    Strings fakeArgv;
    std::vector<char *> fakeArgv2;

    if (std::string(argv[0]).find("mount.fuse.dwarffs") != std::string::npos) {
        assert(argc == 5);
        assert(std::string(argv[3]) == "-o");
        fakeArgv = { argv[0], argv[2], "-o", argv[4] };
        fakeArgv2 = stringsToCharPtrs(fakeArgv);
        argc = fakeArgv.size();
        argv = fakeArgv2.data();
    }

    dwarffs_param params;

    fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &params, dwarffs_opts, dwarffs_opt_proc))
        throw Error("failed to parse options");

    memset(&oper, 0, sizeof(oper));
    oper.getattr = dwarffs_getattr;
    oper.readdir = dwarffs_readdir;
    oper.open = dwarffs_open;
    oper.read = dwarffs_read;

    cacheDir = params.cache ? params.cache : getCacheDir() + "/dwarffs";

    createDirs(cacheDir);

    if (params.uid) {
        if (!params.gid) throw Error("uid requires gid");

        if (auto n = string2Int<uid_t>(params.uid))
            uid = *n;
        else {
            char buf[16384];
            struct passwd pwbuf;
            struct passwd * pw;
            if (getpwnam_r(params.uid, &pwbuf, buf, sizeof(buf), &pw) != 0 || !pw)
                throw Error("cannot look up user '%s'", params.uid);
            uid = pw->pw_uid;
        }

        if (auto n = string2Int<gid_t>(params.gid))
            gid = *n;
        else {
            char buf2[16384];
            struct group grbuf;
            struct group * gr;
            if (getgrnam_r(params.gid, &grbuf, buf2, sizeof(buf2), &gr) != 0 || !gr)
                throw Error("cannot look up group '%s'", params.gid);
            gid = gr->gr_gid;
        }

        if (chown(cacheDir.c_str(), uid, gid))
            throw SysError("setting ownership of '%s'", cacheDir);
    }

    /* Hack: when running under systemd, keep logging to the original
       stderr (i.e. the journal) after fuse daemonizes. */
    bool inSystemd = getEnv("IN_SYSTEMD") == "1";

    int stderrFd = inSystemd ? dup(STDERR_FILENO) : -1;

    struct fuse * fuse;
    char * mountpoint;
    int multithreaded;

    fuse = fuse_setup(args.argc, args.argv, &oper, sizeof(oper), &mountpoint,
        &multithreaded, nullptr);
    if (!fuse) throw Error("FUSE setup failed");

    if (uid != (uid_t) -1) {
        if (setgid(gid) || setuid(uid))
            throw SysError("dropping privileges");
    }

    if (stderrFd != -1)
        dup2(stderrFd, STDERR_FILENO);

    if (multithreaded)
        fuse_loop_mt(fuse);
    else
        fuse_loop(fuse);

    fuse_teardown(fuse, mountpoint);
}

int main(int argc, char * * argv)
{
    return nix::handleExceptions(argv[0], [&]() {
        mainWrapped(argc, argv);
    });
}
