// Self tests for lmdbcols library
// 
// Dies on assert() failure if a test fails (so you can test the exit code).
//
// Creates a new scratch LMDB DB file for testing.
// Provide name/path to this as the first and only cli argument.
// Bails if a file already exists here.

// Copyright (c) Inkblot Software Limited 2017
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <iostream>
#include <cstdint>
#include <cassert>
#include <fstream>

#include <lmdbcols.hpp>

using std::string;
using std::vector;


// ======================================================================
// == Bailing out

namespace {
    const string fs_usage =
        "USAGE: \n"
        "  ./run_tests DB_NAME\n"
        "\n"
        "NB there must be no file present at DB_NAME.\n";

    void bailWithMsgAndUsage( const string &msg ) {
        std::cerr
            << std::endl
            << "========== BAILING: ==========" << std::endl
            << "## " << msg << std::endl
            << fs_usage << std::endl;
        exit(1);
    }
}  // anon namespace


// ======================================================================
// == main()

int main( int argc, char *argv[] ) {
    if(argc != 2) bailWithMsgAndUsage("Bad arg count");
    const string dbName = argv[1];

    {
        std::ifstream testStreamDontUse {dbName.c_str()};
        if(testStreamDontUse) bailWithMsgAndUsage("File exists at that DB name");
    }
    
    lmdbcols::test( dbName );
    
    LMDBCOLS_LOG();
    LMDBCOLS_LOG("All tests finished successfully");
}
