// Copyright (c) Inkblot Software Limited 2017
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <cassert>
#include <vector>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <iostream>

#include <lmdb++.h>


namespace lmdbcols {
    using std::string;

    
    // ======================================================================
    // == "Log these to stderr" macro ===
    // ==
    // ==   We expose this to library clients, as the functionality is generally
    // ==   useful, and it saves them implementing it themselves.
    // ======================================================================

#define LMDBCOLS_LOG(...) lmdbcols::detail::log_h(__FILE__, __LINE__, ##__VA_ARGS__)

    namespace detail {
        template <typename T, typename ...Ts>
        inline void log_h(const char *file, const size_t line, T o1, Ts... os) {
            std::cerr << "#### " << file << ":" << line << "  --  " << o1;
            (void)(int[]){ 0, (std::cerr << ", " << os, void(), 0)... };
            std::cerr << std::endl; }
    
        inline void log_h(const char *file, const size_t line) { log_h(file,line,""); }
    }  // namespace detail


    // ======================================================================
    // == EightPadded ===
    // ==
    // ==   Utility class that packs its member's size to the next full multiple
    // ==   of eight bytes.
    // ==   You probably want to use this if you want to store a type in the DB
    // ==   that doesn't align at a multiple of eight (since our interface types
    // ==   require that).
    // ======================================================================

    template <typename T>
    class alignas(8)
    EightPadded {
        T _data;
        
    public:
        explicit EightPadded() = default;  // NB undefined state, you probably don't want to use this
        explicit EightPadded(T d) {
            memset(this, 0, sizeof(*this));
            _data = d;
        }

        // TODO consider whether I want implicit conversions; I may, for ease of use
        
        T& get() {return _data;}
        
        const T& cget()      const {return _data;}
        const T& operator*() const {return _data;}
    };

    // Tests
    namespace detail {
        struct TestStruct1{ int32_t i; };
        static_assert( sizeof(TestStruct1) == 4, "" );
        static_assert( sizeof(EightPadded<TestStruct1>) == 8, "" );
        static_assert( offsetof(TestStruct1, i) == 0, "" );
    }
        

    // ======================================================================
    // == is_valid_keyval_type ===
    // ==
    // ==   Type test to see whether a given type is suitable for storing in
    // ==   in a DB collection managed by this library.
    // ==
    // ==   Clients don't need to use this directly, as the main classes they
    // ==   use call it in a static_assert to check.
    // ======================================================================
  
    template <class T>
    struct is_valid_keyval_type {
        static constexpr bool value = std::is_pod<T>::value && !(sizeof(T) % 8);
    };

    static_assert( !is_valid_keyval_type<char>::value, "" );
    static_assert(  is_valid_keyval_type<double>::value, "" );
    static_assert(  is_valid_keyval_type<EightPadded<char>>::value, "" );


    // ======================================================================
    // == LmdbSpan ===
    // ==
    // ==   A (const) view into an array of C++ native typed data currently
    // ==   stored in LMDB.
    // ==
    // ==   Untyped / unknown type data in LMDB is represented as an
    // ==   LmdbSpan<unsigned char>.
    // ==
    // ==   Every access the user makes to the db is through one of these
    // ==   (though sometimes we hide this through refs).
    // ======================================================================

    template <typename T>
    class LmdbSpan {
        const T *m_data; size_t m_size;  // size is number of T's

        // Check pointer alignment matches a given type's (is multiple of)
        template<typename TTarg> static bool is_aligned_for(const void *ptr) {
            return !( reinterpret_cast<std::uintptr_t>(ptr) % alignof(TTarg) ); }
    
    public:

        size_t    size()  const {return m_size;}  // Number of T's in span
        const T * begin() const {return m_data;}
        const T * end()   const {return m_data + m_size;}

        explicit LmdbSpan(const T *ptr, size_t count) :m_data{ptr}, m_size{count} {}

        // Range construction - pointers as pair of iterators.
        // 'end' is one past last element in spanned array.
        explicit LmdbSpan(const T *beg, const T *end)
            :m_data{beg}, m_size{static_cast<size_t>(end-beg)}
        {
            assert( end-beg >= 0);
        }

        explicit LmdbSpan(MDB_val mv) :m_data{reinterpret_cast<T*>(mv.mv_data)},
                                       m_size{mv.mv_size/sizeof(T)}
        {
            assert( !(mv.mv_size % sizeof(T)) );
            assert( is_aligned_for<T>(m_data) );
        }

        // Having a 'null' span (zero length, no data) helps api symmetry in clients
        static LmdbSpan<T> makeNull() {return LmdbSpan<T>{nullptr, nullptr};}
        
        bool isNull() const {return m_data == nullptr;}

        // Array index
        const T& operator[](size_t n) const {
            assert(n < m_size);
            return m_data[n];
        }

        // Reinterpret the data within this span as a given type
        template <typename TNew>
        const TNew& asType() const {
            assert(sizeof(TNew) == sizeof(T)*size());
            assert( is_aligned_for<TNew>(m_data) );
            return *reinterpret_cast<const TNew *>(begin());
        }
        
        template <typename TNew>
        LmdbSpan<TNew> asSpan() {
            // FIXME idiomatic cast
            assert( !( (sizeof(T)*m_size) % sizeof(TNew) ) );
            assert( is_aligned_for<TNew>(m_data) );
            return LmdbSpan<TNew>{(TNew*)m_data, m_size/sizeof(TNew)};
        }

        // Get a (usually) smaller span within this one
        LmdbSpan<T> subSpan(size_t offset, size_t count) const {
            assert( offset + count <= size() );
            return LmdbSpan<T>{ begin() + offset, count };
        }
    };
    

    // ======================================================================
    // == DatabaseEnvironment ===
    // ==
    // ==   Simplified wrapper around MDB_env / lmdb::env.
    // ==   Assumes you want a bare db file, rather than a directory.
    // ==
    // ==   Makes reasonably sensible guesses about maxDBs and map size (which
    // ==   you can override).
    // ==   Also lets you open read/readwrite transactions easily.
    // ==
    // ==   NB we round the requested mapsize up to the next multiple of 4096,
    // ==   as the DB requires it and most callers don't care.
    // ======================================================================
    
    class DatabaseEnvironment {
        static constexpr size_t defaultMaxSize = 1UL * 1024UL * 1024UL * 1024UL; // 1GB
        static constexpr size_t defaultMaxDbs = 10;  // TODO consider this

        lmdb::env m_env;
    
    public:
        explicit DatabaseEnvironment(const string &dbPath,
                                     size_t maxSize = defaultMaxSize,
                                     size_t maxDbs = defaultMaxDbs)
                :m_env{lmdb::env::create()}
        {
            m_env.set_mapsize((maxSize % 4096 == 0)
                              ? maxSize
                              : (maxSize + 4096 - (maxSize%4096)));
            m_env.set_max_dbs(maxDbs);
            m_env.open(dbPath.c_str(), MDB_NOSUBDIR, 0664);
        }

        lmdb::txn openWriteTxn() {return lmdb::txn::begin(m_env, nullptr); }
        lmdb::txn openReadTxn()  {return lmdb::txn::begin(m_env, nullptr, MDB_RDONLY);}
    };


    // ======================================================================
    // == DbiWrapper ===
    // ==
    // ==   Wrapper around MDB_dbi / lmdb::dbi, with a bit more functionality
    // ==   that we neeed, as explicitly as possible.
    // ==   Clients are very unlikely to want to use this class.
    // ==
    // ==   NB doesn't hande issues like alignment - just dumbly handles groups
    // ==   of bytes.
    // ======================================================================
    
    class DbiWrapper {
        static constexpr unsigned int default_dbiflags = (MDB_CREATE);
        
        const string m_dbName;
        const unsigned int m_dbiflags = default_dbiflags;
    
        lmdb::dbi dbi(lmdb::txn &txn) {
            // FIXME use manual mdb_dbi_open here, and throw exception
            // if it returns MDB_NOTFOUND error code (for read txns in empty db)
            return lmdb::dbi::open(txn, m_dbName.c_str(), m_dbiflags);
        }
    
    public:
        explicit DbiWrapper(const string &dbName, const unsigned int dbiflags=default_dbiflags)
            :m_dbName{dbName}, m_dbiflags{dbiflags} {}

        // --- Putting
    
        template <typename K, typename V>
        void put(lmdb::txn &txn, const K &key, const V &val) {
            // TODO idiomatic casts
            MDB_val mkey {sizeof(key), (void*)(&key)};
            MDB_val mval {sizeof(val), (void*)(&val)};
            auto rc = mdb_put(txn, dbi(txn), &mkey, &mval, 0);
            if(rc) lmdb::error::raise("DbiWrapper PUT error", rc);
        }

        template <typename K, typename VElem>
        void putArray(lmdb::txn &txn, const K &key, const VElem *dat, size_t count) {
            // TODO idiomatic casts
            MDB_val mkey {sizeof(key), (void*)&key};
            MDB_val mval {sizeof(dat[0])*count, (void*)dat};
            auto rc = mdb_put(txn, dbi(txn), &mkey, &mval, 0);
            if(rc) lmdb::error::raise("DbiWraper PUT ARRAY error", rc);
        }

        // --- Getting
    
        template <typename K>
        LmdbSpan<unsigned char> get(lmdb::txn &txn, const K &key) {
            MDB_val mval;
            // TODO idiomatic cast
            MDB_val mkey {sizeof(key), (void*)&key};
            int rc = mdb_get(txn, dbi(txn), &mkey, &mval);
            if(rc) lmdb::error::raise("DbiWrapper GET error", rc);
            return LmdbSpan<unsigned char>{mval};
        }

        // --- Key presence checking
        template <typename K>
        bool exists(lmdb::txn &txn, const K &key) {
            // TODO idiomatic cast
            MDB_val mkey {sizeof(key), (void*)&key};
            MDB_val mval;
            int rc = mdb_get(txn, dbi(txn), &mkey, &mval);
            auto myDbi = dbi(txn);
            if(rc == MDB_NOTFOUND) return false;
            else if(rc == 0)       return true;
            else lmdb::error::raise("DbiWrapper EXISTS error", rc);
        }
    };
    
    
    // ======================================================================
    // == MapDB_PodPod ===
    // ==
    // ==   Manager for a single named LMDB database (within a db file) that
    // ==   has all keys of the POD type TAllKey, and all values of type TAllVal.
    // ==   (The 'All' bit refers to alignment; see below.)
    // ==
    // ==   Gives you simple get/put access to the contents.
    // ==
    // ==   You are required to provide TAllKey and TAllVal with sizeof % 8,
    // ==   to preserve alignment inside the database.
    // ==
    // ==   See the EightPadded type if you want an easy way of making a suitable
    // ==   type from yours, or MapDB_AutoPadded_Pod_Pod if you want the
    // ==   EightPadded wrapping to be applied automatically.
    // ======================================================================

    template <typename TAllKey, typename TAllVal>
    class MapDB_Pod_Pod {
        static_assert(is_valid_keyval_type<TAllKey>::value, "");
        static_assert(is_valid_keyval_type<TAllVal>::value, "");
        
        DbiWrapper m_dbiWrap;
    
    public:
        explicit MapDB_Pod_Pod(const string &dbName) :m_dbiWrap{dbName} {}
        explicit MapDB_Pod_Pod(const string &dbName, const unsigned int dbiFlags)
            :m_dbiWrap{dbName, dbiFlags} {}
        
        void put(lmdb::txn &txn, const TAllKey &key, const TAllVal &val) {
            m_dbiWrap.put(txn, key, val);
        }
        
        const TAllVal &get(lmdb::txn &txn, const TAllKey &key) {
            LmdbSpan<unsigned char> sp = m_dbiWrap.get(txn, key);
            return sp.asType<TAllVal>();
        }

        // TODO add exists method (and test)
    };


    // ======================================================================
    // == MapDB_AutoPadded_Pod_Pod ===
    // ==
    // ==   Exactly the same as MapDB_Pod_Pod, but wraps your key and value
    // ==   types in an implicit EightPadded, which gets used behind the scenes
    // ==   when you call get() and put();
    // ==
    // ==   We give this a slightly longer name than MapDB_Pod_Pod because you
    // ==   probably want to think about whether the extra padding will have an
    // ==   effect on your space usage and performance.
    // ==   For small/simple uses it really won't.
    // ======================================================================
    
    template <typename TKey, typename TVal>
    class MapDB_AutoPadded_Pod_Pod {
        using PadKey = EightPadded<TKey>;
        using PadVal = EightPadded<TVal>;
        
        MapDB_Pod_Pod<PadKey, PadVal> m_db;
    
    public:
        explicit MapDB_AutoPadded_Pod_Pod(const string &dbName) :m_db{dbName} {}
        explicit MapDB_AutoPadded_Pod_Pod(const string &dbName, const unsigned int dbiFlags)
            :m_db{dbName, dbiFlags} {}
        
        void put(lmdb::txn &txn, const TKey &key, const TVal &val) {
            m_db.put(txn, PadKey{key}, PadVal{val});
        }
        
        const TVal &get(lmdb::txn &txn, const TKey &key) {
            const PadVal &pv = m_db.get(txn, PadKey{key});
            return *pv;
        }

        // TODO add exists() method (and test)
    };


    // ======================================================================
    // == MapDB_Pod_PodArray ===
    // ==
    // ==   Similar to MapDB_Pod_Pod, but stores an array of values
    // ==   rather than a single one (TAllValElem is value element type).
    // ==
    // ==   Similarly, for aligment reasons we require that sizeof(TAllKey) and
    // ==   sizeof(TAllValElem) are both multiples of 8.
    // ==
    // ==   get() returns an LmdbSpan, which is just a normal span/view type,
    // ==   and very easy to use.
    // ======================================================================
    
    template <typename TAllKey, typename TAllValElem>
    class MapDB_Pod_PodArray {
        static_assert(is_valid_keyval_type<TAllKey>::value, "");
        static_assert(is_valid_keyval_type<TAllValElem>::value, "");
        
        DbiWrapper m_dbiWrap;

    public:
        explicit MapDB_Pod_PodArray(const string &dbName) :m_dbiWrap{dbName} {}
        explicit MapDB_Pod_PodArray(const string &dbName, const unsigned int dbiFlags)
            :m_dbiWrap{dbName, dbiFlags} {}
        
        // // TODO consider whether I want this one
        // template <class Range>
        // void put(lmdb::txn &txn, const TAllKey &key, Range range) {
        //   static_assert( std::is_same<
        //                      std::remove_cv_t<TAllValElem>,
        //                      std::remove_cv_t<typename Range::value_type> >::value,
        //                  "Calling array put function with range containing wrong value_type" );
        //   m_dbiWrap.putArray(txn, key, &range[0], range.size()); }
        
        LmdbSpan<TAllValElem>
        get(lmdb::txn &txn, const TAllKey &key) {
            LmdbSpan<unsigned char> sp = m_dbiWrap.get(txn, key);
            return sp.asSpan<TAllValElem>();
        }
        
        void put(lmdb::txn &txn, const TAllKey &key, const TAllValElem *data, size_t count) {
            m_dbiWrap.putArray(txn, key, data, count);
        }
        
        bool exists( lmdb::txn &txn, const TAllKey &key ) {
            return m_dbiWrap.exists(txn, key);
        }
    };


    // ======================================================================
    // == MapDB_AutoPadded_Pod_PodArray ===
    // ==
    // ==   Similar to MapDB_Pod_PodAray, but wraps the user-provided
    // ==   types in EightPadded automatically to comply with alignment reqs.
    // ==
    // ==   Annoyingly, since we return an LmdbSpan on get() giving zero-copy
    // ==   access to the data in the db, you have to run operator*/get()/cget()
    // ==   on the members of the span to strop away the EightPadded before you
    // ==   can work with their actual values - we can't trivially do it
    // ==   automatically.
    // ==   I'll probably bring in some kind of facade or iterator to help with
    // ==   this at some point, if it matters.
    // ======================================================================

    template <typename TKey, typename TValElem>
    class MapDB_AutoPadded_Pod_PodArray {
        using PadKey     = EightPadded<TKey>;
        using PadValElem = EightPadded<TValElem>;
        
        MapDB_Pod_PodArray<PadKey, PadValElem> m_db;

    public:
        explicit MapDB_AutoPadded_Pod_PodArray(const string &dbName) :m_db{dbName} {}
        explicit MapDB_AutoPadded_Pod_PodArray(const string &dbName, const unsigned int dbiFlags)
            :m_db{dbName, dbiFlags} {}
        
        template <typename Range>
        void put(lmdb::txn &txn, const TKey &key, const Range &range) {
            static_assert( std::is_same<
                               typename std::remove_cv<TValElem>::type,
                               typename std::remove_cv<typename Range::value_type>::type
                             >::value,
                           "Calling array put function with range containing wrong value_type" );
      
            std::vector<PadValElem> toPut {};
            toPut.reserve(range.size());
            for(auto &o : range) toPut.push_back(PadValElem{o});
            
            m_db.put(txn, PadKey{key}, &toPut[0], toPut.size());
        }
        
        LmdbSpan<PadValElem>
        get(lmdb::txn &txn, const TKey &key) {
            return m_db.get(txn, PadKey{key});
        }
        
        // TODO write exists method
    };
    

    // ======================================================================
    // == Library self test ===
    // ======================================================================

    // TODO poss test here using the with-lmdb-dbi-flags constructor for some
    //      of the classes.
    // TODO probably also test not-padded classes directly (though we test them
    //      here through the auto-padded versions).
    
    inline void test(const string &testDbPath) {
        // Padding tests
        
        auto epc = EightPadded<char>{'c'};
    
        assert( sizeof(epc) == 8 );
        assert( 'c' == *epc );
        
        const char *ptr = &*epc;
        for(int i=0+1; i<8; ++i) assert(ptr[i] == 0);


        // Map db tests

        auto env   = DatabaseEnvironment{testDbPath};
        auto mapdb = MapDB_AutoPadded_Pod_Pod<int32_t,char>{"mdb_p_p"};
        
        {
            auto txn = env.openWriteTxn();
            mapdb.put(txn, 123, 'a');
            txn.commit();
        }

        {
            auto txn = env.openReadTxn();
            const char &ch = mapdb.get(txn, 123);
            assert( ch == 'a' );
            LMDBCOLS_LOG("## Did DB get, same came back");
        }

        
        // Map db->arr tests
        
        auto arrdb = MapDB_AutoPadded_Pod_PodArray<int32_t, char>{"mdb_p_parr"};
        
        {
            auto txn = env.openWriteTxn();
            auto chars = std::vector<char>{ 'a', 'b', 'c' };
            arrdb.put(txn, 22, chars);
            txn.commit();
            LMDBCOLS_LOG("## Did array db put");
        }

        {
            auto txn = env.openReadTxn();
            auto gotSp = arrdb.get(txn, 22);
            assert( gotSp.size() == 3 );
            assert( *gotSp[1] == 'b' );
            LMDBCOLS_LOG("## Did array fetch from DB, and was what we expected");
        }
        
        // Also test LmdbSpan while we're here
        {
            auto txn = env.openReadTxn();
            auto sp1 = arrdb.get(txn, 22);
            auto sp2 = LmdbSpan<EightPadded<char>>{ sp1.begin(), sp1.end() };
            assert( sp1.size() == sp2.size() );
            assert( sp1.begin() == sp2.begin() );
            assert( sp1.end() == sp2.end() );
        }
        
        LMDBCOLS_LOG("lmdbcols self test completed successfully");
    }
}  // namespace lmdbcols
