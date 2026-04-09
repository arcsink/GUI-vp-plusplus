# Archived version of LuaBridge3 Tag 3.0-beta2 (original Tag is broken) for RISC-V VP++

The original "3.0-beta3" Tag (https://github.com/kunitoki/LuaBridge3/tree/3.0-beta3) has a broken reference to submodule ThirdParty/googletest.

This archived version directly integrates the code for the submodules:
 * ThirdParty/googletest
   https://github.com/kunitoki/googletest.git
   b996fc911bf2df225e1b3a66e3da195c8326ee0d googletest (release-1.8.0-2867-gb996fc91)
 * ThirdParty/luau
   https://github.com/kunitoki/luau.git
   7257f34d89fddfefeedc70cc2023a29851d6d517 luau (0.506-1-g7257f34)

## Background
see also https://github.com/kunitoki/LuaBridge3/issues/203

Tag 3.0-beta3 uses
https://github.com/kunitoki/googletest.git
b996fc911bf2df225e1b3a66e3da195c8326ee0d
as submodule ThirdParty/googletest

However, commit b996fc911bf2df225e1b3a66e3da195c8326ee0d does not (or no longer) exist in https://github.com/kunitoki/googletest.git

```
$ git clone --recursive https://github.com/kunitoki/LuaBridge3.git --branch 3.0-beta3
Cloning into 'LuaBridge3'...
remote: Enumerating objects: 7391, done.
remote: Counting objects: 100% (526/526), done.
remote: Compressing objects: 100% (121/121), done.
remote: Total 7391 (delta 471), reused 406 (delta 403), pack-reused 6865 (from 2)
Receiving objects: 100% (7391/7391), 4.41 MiB | 16.01 MiB/s, done.
Resolving deltas: 100% (4640/4640), done.
Note: switching to '1ddef3950485f10f32500edabe20198f3d90c8b9'.

You are in 'detached HEAD' state. You can look around, make experimental
changes and commit them, and you can discard any commits you make in this
state without impacting any branches by switching back to a branch.

If you want to create a new branch to retain commits you create, you may
do so (now or later) by using -c with the switch command. Example:

  git switch -c <new-branch-name>

Or undo this operation with:

  git switch -

Turn off this advice by setting config variable advice.detachedHead to false

Submodule 'ThirdParty/googletest' (https://github.com/kunitoki/googletest.git) registered for path 'ThirdParty/googletest'
Submodule 'ThirdParty/luau' (https://github.com/kunitoki/luau.git) registered for path 'ThirdParty/luau'
Cloning into '/home/ame/tmp/LUA_TEST/LuaBridge3/ThirdParty/googletest'...
remote: Enumerating objects: 26980, done.        
remote: Total 26980 (delta 0), reused 0 (delta 0), pack-reused 26980 (from 1)        
Receiving objects: 100% (26980/26980), 12.41 MiB | 31.07 MiB/s, done.
Resolving deltas: 100% (20052/20052), done.
Cloning into '/home/ame/tmp/LUA_TEST/LuaBridge3/ThirdParty/luau'...
remote: Enumerating objects: 2327, done.        
remote: Total 2327 (delta 0), reused 0 (delta 0), pack-reused 2327 (from 1)        
Receiving objects: 100% (2327/2327), 6.71 MiB | 28.99 MiB/s, done.
Resolving deltas: 100% (1301/1301), done.
fatal: remote error: upload-pack: not our ref b996fc911bf2df225e1b3a66e3da195c8326ee0d
fatal: Fetched in submodule path 'ThirdParty/googletest', but it did not contain b996fc911bf2df225e1b3a66e3da195c8326ee0d. Direct fetching of that commit failed.
```

# Original README

<a href="https://kunitoki.github.io/LuaBridge3">
<img src="https://github.com/kunitoki/LuaBridge3/blob/master/logo.png?raw=true">
</a>
<a href="https://lua.org">
<img src="https://github.com/kunitoki/LuaBridge3/blob/master/lua.png?raw=true">
</a>
<br>

# LuaBridge 3.0

[LuaBridge][1] is a lightweight and dependency-free library for mapping data,
functions, and classes back and forth between C++ and [Lua][2] (a powerful,
fast, lightweight, embeddable scripting language). LuaBridge has been tested
and works with Lua revisions starting from 5.1.5, and also compatibility is
provided with lua 5.2.4, 5.3.6 and 5.4.3 as well as [LuaJit][3] and [Luau][4].

## Features

LuaBridge is usable from a compliant C++17 compiler and offers the following features:

- [MIT Licensed][5]
- Headers-only: No Makefile, no .cpp files, just one `#include` and one header file (optional) !
- Simple, light, and nothing else needed.
- No macros, settings, or configuration scripts needed.
- Supports different object lifetime management models.
- Convenient, type-safe access to the Lua stack.
- Automatic function parameter type binding.
- Easy access to Lua objects like tables and functions.
- Interoperable with most common c++ standard library container types.
- Can work with both c++ exceptions and without (Works with `-fno-exceptions` and `/EHsc-`).
- Written in a clear and easy to debug style.

## Status

![Build MacOS](https://github.com/kunitoki/LuaBridge3/workflows/Build%20MacOS/badge.svg?branch=master)
![Build Windows](https://github.com/kunitoki/LuaBridge3/workflows/Build%20Windows/badge.svg?branch=master)
![Build Linux](https://github.com/kunitoki/LuaBridge3/workflows/Build%20Linux/badge.svg?branch=master)

## Code Coverage
[![Coverage Status](https://coveralls.io/repos/github/kunitoki/LuaBridge3/badge.svg?branch=master)](https://coveralls.io/github/kunitoki/LuaBridge3?branch=master)

## Documentation

Please read the [LuaBridge Reference Manual][6] for more details on the API.

## Release Notes

Plase read the [LuaBridge Release Notes][7] for more details

## Unit Tests

Unit test build requires a CMake and C++17 compliant compiler.

There are 9 unit test flavors:
* `LuaBridgeTests51` - uses Lua 5.1
* `LuaBridgeTests51Noexcept` - uses Lua 5.1 without exceptions enabled
* `LuaBridgeTests52` - uses Lua 5.2
* `LuaBridgeTests52Noexcept` - uses Lua 5.2 without exceptions enabled
* `LuaBridgeTests53` - uses Lua 5.3
* `LuaBridgeTests53Noexcept` - uses Lua 5.3 without exceptions enabled
* `LuaBridgeTests54` - uses Lua 5.4
* `LuaBridgeTests54Noexcept` - uses Lua 5.4 without exceptions enabled
* `LuaBridgeTestsLuau` - uses Luau

(Luau compiler needs exceptions, so there is no tests that runs on Luau without exceptions)

Generate Unix Makefiles and build on Linux:
```bash
git clone --recursive git@github.com:kunitoki/LuaBridge3.git

mkdir -p LuaBridge/build
pushd LuaBridge/build
cmake -G "Unix Makefiles" ../
cmake --build . -DCMAKE_BUILD_TYPE=Debug
# or cmake --build . -DCMAKE_BUILD_TYPE=Release
# or cmake --build . -DCMAKE_BUILD_TYPE=RelWithDebInfo
popd
```

Generate XCode project and build on MacOS:
```bash
git clone --recursive git@github.com:kunitoki/LuaBridge3.git

mkdir -p LuaBridge/build
pushd LuaBridge/build
cmake -G Xcode ../ # Generates XCode project build/LuaBridge.xcodeproj
cmake --build . -DCMAKE_BUILD_TYPE=Debug
# or cmake --build . -DCMAKE_BUILD_TYPE=Release
# or cmake --build . -DCMAKE_BUILD_TYPE=RelWithDebInfo
popd
```

Generate VS2019 solution on Windows:
```cmd
git clone --recursive git@github.com:kunitoki/LuaBridge3.git

mkdir LuaBridge/build
pushd LuaBridge/build
cmake -G "Visual Studio 16" ../ # Generates MSVS solution build/LuaBridge.sln
popd
```

## Official Repository

LuaBridge is published under the terms of the [MIT License][5].

The original version of LuaBridge was written by Nathan Reed. The project has
been taken over by Vinnie Falco, who added new functionality, wrote the new
documentation, and incorporated contributions from Nigel Atkinson. Then it has
been forked from the original https://github.com/vinniefalco/LuaBridge into its
own LuaBridge3 repository by Lucio Asnaghi, and development continued there.

For questions, comments, or bug reports feel free to open a Github issue
or contact Lucio Asnaghi directly at the email address indicated below.

Copyright 2020, Lucio Asnaghi (<kunitoki@gmail.com>)<br>
Copyright 2019, Dmitry Tarakanov<br>
Copyright 2012, Vinnie Falco (<vinnie.falco@gmail.com>)<br>
Copyright 2008, Nigel Atkinson<br>
Copyright 2007, Nathan Reed<br>

[1]:  https://github.com/kunitoki/LuaBridge3 "LuaBridge"
[2]:  https://lua.org "The Lua Programming Language"
[3]:  https://luajit.org/ "The LuaJIT Project"
[4]:  https://luau-lang.org/ "The Luau Project"
[5]:  https://www.opensource.org/licenses/mit-license.html "The MIT License"
[6]:  https://kunitoki.github.io/LuaBridge3/Manual "LuaBridge Reference Manual"
[7]:  https://kunitoki.github.io/LuaBridge3/CHANGES "LuaBridge Release Notes"
