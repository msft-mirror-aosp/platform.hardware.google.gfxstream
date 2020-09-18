
class VkEncoder::Impl {
public:
    Impl(IOStream* stream) : m_stream(stream), m_logEncodes(false) {
        m_stream.incStreamRef();
        const char* emuVkLogEncodesPropName = "qemu.vk.log";
        char encodeProp[PROPERTY_VALUE_MAX];
        if (property_get(emuVkLogEncodesPropName, encodeProp, nullptr) > 0) {
            m_logEncodes = atoi(encodeProp) > 0;
        }
    }

    ~Impl() {
        m_stream.decStreamRef();
    }

    VulkanCountingStream* countingStream() { return &m_countingStream; }
    VulkanStreamGuest* stream() { return &m_stream; }
    Pool* pool() { return &m_pool; }
    ResourceTracker* resources() { return ResourceTracker::get(); }
    Validation* validation() { return &m_validation; }

    void log(const char* text) {
        if (!m_logEncodes) return;
        ALOGD("encoder log: %s", text);
    }

    void flush() {
        lock();
        m_stream.flush();
        unlock();
    }

    // can be recursive
    void lock() {
        if (this == sAcquiredEncoderThreadLocal) {
            ++sAcquiredEncoderThreadLockLevels;
            return; // recursive
        }
        while (mLock.test_and_set(std::memory_order_acquire));
        sAcquiredEncoderThreadLocal = this;
        sAcquiredEncoderThreadLockLevels = 1;
    }

    void unlock() {
        if (this != sAcquiredEncoderThreadLocal) {
            // error, trying to unlock without having locked first
            return;
        }

        --sAcquiredEncoderThreadLockLevels;
        if (0 == sAcquiredEncoderThreadLockLevels) {
            mLock.clear(std::memory_order_release);
            sAcquiredEncoderThreadLocal = nullptr;
        }
    }

    void incRef() {
        __atomic_add_fetch(&m_refCount, 1, __ATOMIC_SEQ_CST);
    }

    bool decRef() {
        if (0 == __atomic_sub_fetch(&m_refCount, 1, __ATOMIC_SEQ_CST)) {
            return true;
        }
        return false;
    }

private:
    VulkanCountingStream m_countingStream;
    VulkanStreamGuest m_stream;
    Pool m_pool { 8, 4096, 64 };

    Validation m_validation;
    bool m_logEncodes;
    std::atomic_flag mLock = ATOMIC_FLAG_INIT;
    static thread_local Impl* sAcquiredEncoderThreadLocal;
    static thread_local uint32_t sAcquiredEncoderThreadLockLevels;
    uint32_t m_refCount = 1;
};

VkEncoder::~VkEncoder() { }

// static
thread_local VkEncoder::Impl* VkEncoder::Impl::sAcquiredEncoderThreadLocal = nullptr;
thread_local uint32_t VkEncoder::Impl::sAcquiredEncoderThreadLockLevels = 0;

struct EncoderAutoLock {
    EncoderAutoLock(VkEncoder* enc) : mEnc(enc) {
        mEnc->lock();
    }
    ~EncoderAutoLock() {
        mEnc->unlock();
    }
    VkEncoder* mEnc;
};

VkEncoder::VkEncoder(IOStream *stream) :
    mImpl(new VkEncoder::Impl(stream)) { }

void VkEncoder::flush() {
    mImpl->flush();
}

void VkEncoder::lock() {
    mImpl->lock();
}

void VkEncoder::unlock() {
    mImpl->unlock();
}

void VkEncoder::incRef() {
    mImpl->incRef();
}

bool VkEncoder::decRef() {
    if (mImpl->decRef()) {
        delete this;
        return true;
    }
    return false;
}

