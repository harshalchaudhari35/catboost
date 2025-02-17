

RECURSE(
    accurate_accumulate
    accurate_accumulate/benchmark
    accurate_accumulate/benchmark/metrics
    archive/ut
    binsaver
    binsaver/ut_util
    binsaver/ut
    blockcodecs
    blockcodecs/fuzz
    blockcodecs/ut
    build_info
    cgiparam
    cgiparam/fuzz
    cgiparam/ut
    charset
    charset/ut
    chromium_trace
    chromium_trace/benchmark
    chromium_trace/examples
    chromium_trace/ut
    colorizer
    colorizer/ut
    comptrie
    comptrie/loader
    comptrie/loader/ut
    comptrie/ut
    comptrie/benchmark
    containers
    coroutine
    cppparser
    cpuid_check
    dbg_output
    dbg_output/ut
    deprecated
    diff
    diff/ut
    digest
    dns
    dns/ut
    dot_product
    dot_product/bench
    dot_product/ut
    enumbitset
    enumbitset/ut
    fast_exp
    fast_exp/benchmark
    fast_exp/ut
    fast_log
    float16
    float16/ut
    getopt
    getopt/last_getopt_demo
    getopt/small
    getopt/ut
    grid_creator
    grid_creator/fuzz
    grid_creator/ut
    http
    json
    json/flex_buffers
    json/flex_buffers/ut
    json/fuzzy_test
    json/ut
    json/writer/ut
    json/yson
    json/yson/ut
    langmask
    langmask/proto
    langmask/python
    langmask/serialization
    langmask/ut
    langs
    langs/ut
    lcs
    lcs/ut
    lfalloc
    lfalloc/dbg
    lfalloc/dbg_info
    lfalloc/yt
    linear_regression
    linear_regression/benchmark
    linear_regression/ut
    logger
    logger/global
    logger/global/ut
    logger/ut
    malloc
    neh
    neh/asio/ut
    neh/ut
    netliba
    object_factory
    object_factory/ut
    on_disk
    openssl
    packers
    packers/ut
    par
    pop_count
    pop_count/benchmark
    pop_count/ut
    python
    resource
    resource/ut
    sse
    statistics
    statistics/ut
    streams
    string_utils
    svnversion
    svnversion/java
    terminate_handler
    terminate_handler/sample
    testing
    text_processing
    threading
    token
    token/serialization
    token/serialization/ut
    token/ut
    tokenizer
    tokenizer/ut
    unicode
    unittest
    unittest/fat
    unittest/main
    unittest/ut
    yson
    yson/node
    yson/node/pybind
    yson/node/ut
)

IF (OS_LINUX)
    RECURSE(
    
)
ENDIF()

IF (OS_WINDOWS)
    RECURSE(
    
)
ELSE()
    RECURSE(
    sse/ut
)
ENDIF()

IF (NOT SANITIZER_TYPE)
    RECURSE(
    
)
ENDIF()

CHECK_DEPENDENT_DIRS(
    ALLOW_ONLY
    library
    contrib
    util
    yweb/config
)

NEED_CHECK()
