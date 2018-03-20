# dwarffs

`dwarffs` is a FUSE-based filesystem that fetches DWARF debug info
files automatically from `cache.nixos.org`, based on the build ID
embedded in ELF executables and libraries.

## Usage

### NixOS

To enable `dwarffs`, add the following to your `configuration.nix`:

```
imports = [ (builtins.fetchGit https://github.com/edolstra/dwarffs + "/module.nix") ];
```

This creates an automount unit on `/run/dwarffs`. It also sets the
environment variable `NIX_DEBUG_INFO_DIRS` to `/run/dwarffs`. The
`gdb` and `elfutils` packages in Nixpkgs have been patched to use that
environment variable, allowing them to use `dwarffs` automatically.

For example, to get line number information from `eu-stack` for a
running process:

```
$ eu-stack -p 28080 -s
PID 28080 - process
TID 28080:
#0  0x00007f1ee343f99d read
    ../sysdeps/unix/syscall-template.S:84
#1  0x00007f1ee4147864 nix::FdSource::readUnbuffered(unsigned char*, unsigned long)
    /nix/store/50jw5m7lda3rylirxyly9diy55lh149z-glibc-2.25-49-dev/include/bits/unistd.h:44
#2  0x00007f1ee4146d66 nix::BufferedSource::read(unsigned char*, unsigned long)
    src/libutil/serialise.cc:94
...
```

Similarly, to get line number information from a core dump:

```
$ eu-stack --core ./core  -s
PID 22154 - core
TID 22154:
#0  0x00007fd98db9bfcd __read
    ../sysdeps/unix/syscall-template.S:84
#1  0x000000000041cf13 linenoise
    /nix/store/50jw5m7lda3rylirxyly9diy55lh149z-glibc-2.25-49-dev/include/bits/unistd.h:44
#2  0x0000000000471f98 nix::NixRepl::getLine(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)
    src/nix/repl.cc:187
...
```

`gdb` should also obtain debug info automatically:

```
$ gdb -p 22178
(gdb) bt
#0  0x00007fe96642436f in futex_wait_cancelable (private=<optimized out>, expected=0, futex_word=0x7fffa120e3f8) at ../sysdeps/unix/sysv/linux/futex-internal.h:88
#1  __pthread_cond_wait_common (abstime=0x0, mutex=0x7fffa120e300, cond=0x7fffa120e3d0) at pthread_cond_wait.c:502
#2  __pthread_cond_wait (cond=0x7fffa120e3d0, mutex=0x7fffa120e300) at pthread_cond_wait.c:655
#3  0x00007fe966c0dd1c in std::condition_variable::wait(std::unique_lock<std::mutex>&) () from target:/nix/store/lc9cdddv2xv45ighz8znsanjfgkcdgbx-gcc-5.4.0-lib/lib/libstdc++.so.6
#4  0x00007fe96712a044 in nix::Sync<nix::ThreadPool::State, std::mutex>::Lock::wait (cv=..., this=0x7fffa120e0c0) at src/libutil/sync.hh:56
#5  nix::ThreadPool::process (this=this@entry=0x7fffa120e2f0) at src/libutil/thread-pool.cc:60
#6  0x00007fe96747c306 in nix::Store::queryMissing (this=this@entry=0xfc57b0, targets=..., willBuild_=..., willSubstitute_=..., unknown_=..., downloadSize_=@0x7fffa120e500: 0, narSize_=@0x7fffa120e508: 0) at src/libstore/misc.cc:237
#7  0x0000000000415672 in opServe (opFlags=..., opArgs=...) at src/nix-store/nix-store.cc:836
#8  0x000000000040b632 in <lambda()>::operator()(void) const (__closure=<optimized out>) at src/nix-store/nix-store.cc:1070
#9  0x00007fe967720642 in std::function<void ()>::operator()() const (this=0x7fffa120f6f0) at /nix/store/2m6v6lnmnf9521ixd5v0x4qrmjkn30ws-gcc-5.4.0/include/c++/5.4.0/functional:2267
#10 nix::handleExceptions(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::function<void ()>) (programName=..., fun=...) at src/libmain/shared.cc:271
#11 0x00000000004079b7 in main (argc=3, argv=0x7fffa120f818) at src/nix-store/nix-store.cc:1071

(gdb) select-frame 5
(gdb) info locals
state = {s = 0x7fffa120e300, lk = {_M_device = 0x7fffa120e300, _M_owns = true}}
(gdb) p state.s.data.active
$2 = 0
```

## Operation

ELF binaries have a unique build ID embedded in them, allowing
separate debug info files to be looked up. For example, here is the
build ID of a particular build of `libnixexpr.so`:

```
$ readelf -a /nix/store/c0srp6xfqyrjrmqhd1pgw6hcrhcghg87-nix-1.12pre5619_346aeee1/lib/libnixexpr.so | grep 'Build ID'
    Build ID: bde350fa1f1bbde3649bfce3ae143b87683d8bf9
```

Given this build ID, `gdb` and `elfutils` will look in
`/run/dwarffs/.build-id/bd/e350fa1f1bbde3649bfce3ae143b87683d8bf9.debug`
to obtain the debug info for this binary. `dwarffs` will then look in
`https://cache.nixos.org/debuginfo/<build-ID>`:

```
$ curl https://cache.nixos.org/debuginfo/bde350fa1f1bbde3649bfce3ae143b87683d8bf9
{"archive":"../nar/0jz52hawk8wida5b2544i484mws9ddkl8jjpjmw6rzrjf83jr5zw.nar.xz","member":"lib/debug/.build-id/bd/e350fa1f1bbde3649bfce3ae143b87683d8bf9.debug"}
```

That is, the debug info is contained in the NAR file
`https://cache.nixos.org/nar/0jz52hawk8wida5b2544i484mws9ddkl8jjpjmw6rzrjf83jr5zw.nar.xz`
(along with the debug info for other binaries from the same
package). `dwarffs` will unpack all these debug info files to a cache
(`/var/cache/dwarffs/`) and serve them from there.
