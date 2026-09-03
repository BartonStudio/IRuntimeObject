#pragma once

#include <cstdio>
#include <cstdlib>

// 轻量测试断言：失败时输出位置并以非零码退出，供 CTest 判定。
#define TEST_CHECK(condition)                                                                  \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "TEST_CHECK 失败 %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)
