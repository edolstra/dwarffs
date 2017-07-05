#include <iostream>
#include <cstring>
#include <regex>

#include <logging.hh>
#include <shared.hh>
#include <download.hh>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define FUSE_USE_VERSION 30
#include <fuse.h>

using namespace nix;

typedef std::vector<std::string> PathSeq;

PathSeq readmePath{"README"};
static std::string readmeText = "Fnord";

PathSeq buildidPath{"lib", "debug", ".build-id"};

std::regex debugFileRegex("^[0-9a-f]{38}\\.debug$");

std::vector<std::string> debugInfoServers{"http://127.0.0.5/serve-dwarffs"};

Path cacheDir = "/tmp/raw-dwarffs";

struct DebugFile
{
    Path path;
    size_t size;
    AutoCloseFD fd;
};

static std::map<std::string, std::shared_ptr<DebugFile>> files;

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

std::shared_ptr<DebugFile> haveDebugFile(const std::string & buildId)
{
    auto i = files.find(buildId);

    if (i != files.end()) {
        assert(i->second);
        return i->second;
    }

    auto path = cacheDir + "/" + buildId;
    printError("CHECK %s", path);

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) return nullptr;
        auto file = std::make_shared<DebugFile>();
        file->path = path;
        file->size = st.st_size;
        return file;
    }

    if (errno != ENOENT) return nullptr;

    printError("CHECK SERVER %s", path);

    for (auto & server : debugInfoServers) {
        DownloadRequest req(server + "/" + buildId);
        printInfo("checking '%s'", req.uri);
        try {
            auto res = getDownloader()->download(req);
            assert(res.data);
            writeFile(path, *res.data);
            auto file = std::make_shared<DebugFile>();
            file->path = path;
            file->size = res.data->size();
            return file;
        } catch (DownloadError & e) {
            if (e.error != Downloader::NotFound)
                printError("while downloading '%s': %s", req.uri, e.what());
        }
    }

    return nullptr;
}

static int dwarffs_getattr(const char * path_, struct stat * stbuf)
{
    int res = 0;

    memset(stbuf, 0, sizeof(struct stat));

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
}

static int dwarffs_readdir(const char * path_, void * buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info * fi)
{
    auto path = tokenizeString<PathSeq>(path_, "/");

    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    if (path == PathSeq{}) {
        filler(buf, "README", nullptr, 0);
        filler(buf, "lib", nullptr, 0);
    }
    else if (path == PathSeq{"lib"}) {
        filler(buf, "debug", nullptr, 0);
    }
    else if (path == PathSeq{"lib", "debug"}) {
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
    auto path = tokenizeString<PathSeq>(path_, "/");

    if (path != readmePath
        && !(isDebugFile(path) && haveDebugFile(toBuildId(path))))
        return -ENOENT;

    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;

    return 0;
}

static int dwarffs_read(const char * path_, char * buf, size_t size, off_t offset,
    struct fuse_file_info * fi)
{
    auto path = tokenizeString<PathSeq>(path_, "/");

    std::shared_ptr<DebugFile> file;

    if (path == readmePath) {
        auto len = readmeText.size();
        if (offset < len) {
            if (offset + size > len)
                size = len - offset;
            memcpy(buf, readmeText.data() + offset, size);
            return size;
        } else
            return 0;
    }

    else if (isDebugFile(path) && (file = haveDebugFile(toBuildId(path)))) {

        if (file->fd.get() == -1) {
            file->fd = open(file->path.c_str(), O_RDONLY);
            if (file->fd.get() == -1) return -EIO;
        }

        if (lseek(file->fd.get(), offset, SEEK_SET) == -1)
            return -EIO;

        return read(file->fd.get(), buf, size);
    }

    else
        return -ENOENT;

}

static void mainWrapped(int argc, char * * argv)
{
    createDirs(cacheDir);

    fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_operations oper;
    memset(&oper, 0, sizeof(oper));
    oper.getattr = dwarffs_getattr;
    oper.readdir = dwarffs_readdir;
    oper.open = dwarffs_open;
    oper.read = dwarffs_read;

    fuse_main(args.argc, args.argv, &oper, nullptr);
}

int main(int argc, char * * argv)
{
    return nix::handleExceptions(argv[0], [&]() {
        mainWrapped(argc, argv);
    });
}
