// The C scaffold (fuse/fuse.h) and the socket-style SDK (fuse/sdk.h) used to
// define conflicting `enum fuse_status` types, so no translation unit could
// include both. This file compiling is the test.

#include "fuse/fuse.h"
#include "fuse/sdk.h"

#include <gtest/gtest.h>

TEST(Headers, ScaffoldAndSdkCoexist) {
    EXPECT_EQ(static_cast<int>(FUSE_RESULT_SOCKET), -5);
    EXPECT_EQ(static_cast<int>(FUSE_ERR_SOCKET), -2);
}
