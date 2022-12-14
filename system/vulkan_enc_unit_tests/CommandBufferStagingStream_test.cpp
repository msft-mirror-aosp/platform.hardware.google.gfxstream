#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <CommandBufferStagingStream.h>
#include <string.h>

#include <string_view>

static constexpr size_t kTestBufferSize = 1048576;

// tests allocBuffer can successfully allocate a buffer of given size
TEST(CommandBufferStagingStreamTest, AllocateBufferTestUnderMinSize) {
    CommandBufferStagingStream stream;
    unsigned char* buffer = static_cast<unsigned char*>(stream.allocBuffer(kTestBufferSize));
    EXPECT_THAT(buffer, testing::NotNull());
}

// test reallocate buffer remembers previously committed buffers
TEST(CommandBufferStagingStreamTest, ReallocateBufferTest) {
    CommandBufferStagingStream stream;
    unsigned char* buffer = static_cast<unsigned char*>(stream.allocBuffer(kTestBufferSize));
    EXPECT_THAT(buffer, testing::NotNull());

    // write some data
    const std::string_view commandData{"some command"};
    const size_t dataSize = commandData.size();
    memcpy(buffer, commandData.data(), dataSize);

    // commit data
    stream.commitBuffer(dataSize);

    // this will result in a reallocation
    buffer = static_cast<unsigned char*>(stream.allocBuffer(kTestBufferSize));
    EXPECT_THAT(buffer, testing::NotNull());

    // get stream data
    unsigned char* outBuf = nullptr;
    size_t outSize = 0;
    stream.getWritten(&outBuf, &outSize);

    std::string_view actualData{reinterpret_cast<char*>(outBuf), outSize};
    EXPECT_THAT(actualData, testing::Eq(commandData));
}

// tests commitBuffer tracks the portion of the buffer to be committed
TEST(CommandBufferStagingStreamTest, CommitBufferTest) {
    CommandBufferStagingStream stream;
    // allocate buffer
    unsigned char* buffer = static_cast<unsigned char*>(stream.allocBuffer(kTestBufferSize));

    // write some arbitrary data
    const std::string_view commandData{"some command"};
    const size_t dataSize = commandData.size();
    memcpy(buffer, commandData.data(), dataSize);

    // commit
    stream.commitBuffer(dataSize);

    // writeBuffer should have the committed portion in CommandBufferStagingStream
    unsigned char* outBuf = nullptr;
    size_t outSize = 0;
    stream.getWritten(&outBuf, &outSize);

    std::string_view actualData{reinterpret_cast<char*>(outBuf), outSize};
    EXPECT_THAT(actualData, testing::Eq(commandData));
}

TEST(CommandBufferStagingStreamTest, ResetTest) {
    CommandBufferStagingStream stream;
    // allocate buffer
    unsigned char* buffer = static_cast<unsigned char*>(stream.allocBuffer(kTestBufferSize));

    // write some arbitrary data
    const std::string_view commandData{"some command"};
    const size_t dataSize = commandData.size();
    memcpy(buffer, commandData.data(), dataSize);

    // commit
    stream.commitBuffer(dataSize);

    // reset
    stream.reset();

    size_t outSize;
    unsigned char* outBuf;
    // write buffer
    stream.getWritten(&outBuf, &outSize);

    // outSize should be 0 after reset
    EXPECT_EQ(outSize, 0) << "no data should be available for a write after a reset";
}

TEST(CommandBufferStagingStreamDeathTest, UnsupportedAPIs) {
    CommandBufferStagingStream stream;

    EXPECT_DEATH(
        {
            void* buffer;
            size_t size;
            stream.readFully(buffer, size);
        },
        "")
        << "readFully should not be supported";

    EXPECT_DEATH(
        {
            void* buffer;
            size_t size;
            stream.read(buffer, &size);
        },
        "")
        << "read should not be supported";

    EXPECT_DEATH(
        {
            void* buffer;
            size_t size;
            stream.writeFully(buffer, size);
        },
        "")
        << "writeFully should not be supported";

    EXPECT_DEATH(
        {
            void* buffer;
            size_t size;
            size_t len;
            stream.commitBufferAndReadFully(size, buffer, len);
        },
        "")
        << "commitBufferAndReadFully should not be supported";
}