#include <iostream>
#include <cstring>
#include <regex>

#include "logging.hh"
#include "shared.hh"
#include "download.hh"
#include "archive.hh"
#include "compression.hh"
#include "fs-accessor.hh"
#include "nar-accessor.hh"
#include "sync.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
    AutoCloseFD fd;
    DebugFile(const Path & path, size_t size)
        : path(path), size(size)
    { }
};

static Sync<std::map<std::string, std::shared_ptr<DebugFile>>> files_;

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
        DownloadRequest req(canonPath(uri));
        debug("downloading '%s'", uri);

        try {

            auto res = getDownloader()->download(req);
            assert(res.data);

            /* Decompress .xz files. */
            if (std::string(*res.data, 0, 5) == "\xfd" "7zXZ") {
                res.data = decompress("xz", *res.data);
            }

            /* If this is an ELF file, assume it's the raw debug info
               file. */
            if (std::string(*res.data, 0, 4) == "\x7f" "ELF") {
                debug("got ELF debug info file for '%s' from '%s'", buildId, uri);
                writeFile(path, *res.data);
                return std::make_shared<DebugFile>(path, res.data->size());
            }

            /* If this is a JSON file, assume it's a redirect
               file. This is used in cache.nixos.org to redirect to
               the NAR file containing the debug info files for a
               particular store path. */
            else if (std::string(*res.data, 0, 1) == "{") {
                auto json = nlohmann::json::parse(*res.data);

                // FIXME
                std::string archive = json["archive"];
                auto uri2 = dirOf(uri) + "/" + archive;
                return tryUri(uri2);
            }

            /* If this is a NAR file, extract all debug info file,
               not just the one we need right now. After all, disk
               space is cheap but latency isn't. */
            else if (hasPrefix(*res.data, std::string("\x0d\x00\x00\x00\x00\x00\x00\x00", 8) + narVersionMagic1)) {

                auto accessor = makeNarAccessor(make_ref<std::string>(*res.data));

                std::function<void(const Path &)> doPath;

                std::regex debugFileRegex("^/\\.build-id/[0-9a-f]{2}/[0-9a-f]{38}\\.debug$");
                std::string debugFilePrefix = "/.build-id/";

                doPath = [&](const Path & curPath) {
                    auto st = accessor->stat(curPath);

                    if (st.type == FSAccessor::Type::tDirectory) {
                        for (auto & name : accessor->readDirectory(curPath))
                            doPath(curPath + "/" + name);
                    }

                    else if (st.type == FSAccessor::Type::tRegular && std::regex_match(curPath, debugFileRegex)) {
                        std::string buildId2 =
                            std::string(curPath, debugFilePrefix.size(), 2)  +
                            std::string(curPath, debugFilePrefix.size() + 3, 38);

                        debug("got ELF debug info file for '%s' from NAR at '%s'", buildId2, uri);

                        writeFile(cacheDir + "/" + buildId2, accessor->readFile(curPath));
                    }
                };

                doPath("");

                /* Check if we actually got the debug info file we
                   want. */
                return haveDebugFileUncached(buildId, false);
            }

            printError("got unsupported data from '%s'", uri);
            return std::shared_ptr<DebugFile>();

        } catch (DownloadError & e) {
            if (e.error != Downloader::NotFound)
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
        ignoreException();
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
        ignoreException();
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

    } catch (std::exception & e) {
        ignoreException();
        return -EIO;
    }
}

static void mainWrapped(int argc, char * * argv)
{
    verbosity = lvlDebug;

    cacheDir = getCacheDir() + "/dwarffs/buildid";

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
