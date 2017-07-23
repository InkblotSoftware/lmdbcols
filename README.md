lmdbcols
========

C++ templated collections library for the LMDB database

Trivial ultra low-latency zero-copy storage for anywhere from small to
very large volumes of data.

Header only library, for easy inclusion in projects: just put `./include` on your
compiler include path.

Requires C++11 or later.
Tested under 64 bit Linux and Windows msys2; should work fine with MSVC.


Example
-------

We typically map a POD class to another POD class, or a POD class to an
array of POD classes; both are quite similar.

Here we demonstrate the latter:

```cpp
#include <lmdbcols.hpp>

// DB file on disk is "my_data.db"; created if doesn't yet exist
// (Sets max DB file size to 1GB; see ctr definition if you want another value)
auto env = lmdbcols::DatabaseEnvironment{ "my_data.db" };

// Key and Value types to store in the DB
using PathID = uint64_t;
struct GeoPos {double lat, lon;};

// Store our data in a named LMDB database (in the file) called "paths"
auto pathsDB = lmdbcols::MapDB_Pod_PodArray<PathID, GeoPos>{ "paths" };

// Write path
{
    // All interaction with the db happens through transactions
    auto txn = env.openWriteTxn();

    // You can put() from any contiguous array in memory
    auto onePath = std::vector<GeoPos>{ {0,0}, {1,0}, {1,1}, {0,0} };
    pathsDB.put( txn, 123, &onePath[0], onePath.size() );

    // RAII calls abort() on txn if you don't commit before scope end
    txn.commit();
}

// Read path
{
    // Use read transactions whenever possible, for perf
    auto txn = env.openReadTxn();

    // Note that the data returned here is memory mapped, so only materialised
    // from disk to RAM when you dereference a pointer inside it
    auto path = pathsDB.get( txn, 123 );

    // This obviously materialises everything
    std::for_each( std::begin(path), std::end(path),
                   [](auto &gp){
                       printf("%f, %f\n", gp.lat, gp.lon);} );
}
```

You can find a lot more detais as to what's going on in the comments in lmdbcols.hpp.


Alignment issues
----------------

By default LMDB offers very loose alignment guarantees on where keys and values
are stored in memory.

In practice, since keys and values are stored contiguously,  you can impose
stricter guarandees by making sure that every key and every value in a single
DB has as size being a multiple of the required value.

We take 8 bytes as a goldilocks position, so to use the standard MapDB_xxx classes
the types you template them with must have sizeof a multiple of 8.

If this isn't easy to do, see the EightPadded type, which will pad another type
out to the nearest 8 byte boundary with zeros.

Or just use the _AutoPadded_ variants of the MapDB classes, which transparently
use the EightPadded wrapper for you. See the (extensive) comments in lmdbcols.hpp
if you want more information.

Note that e.g. if you have a `long double` in a struct that's your value type,
you'll need to make sure yourself that the key type is also a multiple of 16
in size.


Ownership and license
---------------------

All project files are copyright (c) 2017 Inkblot Software Limited.

This project is released under the Mozilla Public License v2.0.

This means you can include the library in your projects (static or dynamic
linking) without having to change your license or reveal your code, just like
MIT.

But if you want to modify this library itself, all the derived versions of it
stay under MPLv2.


Background and dependencies
---------------------------

[LMDB](https://symas.com/lightning-memory-mapped-database/)
(or [github](https://github.com/LMDB/lmdb))
is an embedded multi-database key-value store, written in C, which provides
the best performance for most read-biased workloads, by some margin.
The API is very clean, but not ideal for use 

(lmdbxx)[https://github.com/bendiken/lmdbxx/]
provides a superb C++ wrapper around LMDB, taking advantage of both RAII
and exceptions. It doesn't impose any specific structure on how you should
store data in the DB.

You need both to build with lmdbcols:

- LMDB is easiest found in distro package managers.
  Otherwise it's just a few source files, so you can easily download the source
  and include the files in your build process: https://github.com/LMDB/lmdb

- lmdbxx is a header only C++ library. For convenience we bundle it with
  lmdbcols, so just add `libs/lmdbxx` to your compiler include path


Testing
-------

The provided CMakeLists.txt file will build a program called run_tests.

Build and run this with a file path to a not-yet-existing scratch db
for its first argument. Test failures register as assert() failures.

