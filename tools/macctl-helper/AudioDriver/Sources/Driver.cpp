#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

namespace {

constexpr UInt32 kSampleRate = 48000;
constexpr UInt32 kChannelCount = 1;
constexpr UInt32 kRingMagic = 0x41445641; // "ADVA"
constexpr UInt32 kRingVersion = 1;
constexpr UInt32 kRingCapacityFrames = 48000 * 5;
constexpr const char* kRingPath = "/tmp/advctl_audio_pcm.ring";

struct RingHeader {
    UInt32 magic;
    UInt32 version;
    UInt32 capacityFrames;
    UInt32 demand;
    UInt32 runningClients;
    UInt32 reserved;
    UInt32 reserved2;
    UInt32 reserved3;
    UInt64 writeIndex;
    UInt64 readIndex;
    UInt64 underrunCount;
    UInt64 overrunCount;
};

static_assert(sizeof(RingHeader) == 64, "ADVCtl ring header must stay ABI-compatible with ADVCtl.app");

class ADVCtlHandler : public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
    ADVCtlHandler()
    {
        EnsureRing();
        SetDemand(0);
    }

    ~ADVCtlHandler() override
    {
        SetDemand(0);
        if (mapped_ != nullptr) {
            munmap(mapped_, mapSize_);
        }
    }

    OSStatus OnStartIO() override
    {
        runningClients_ += 1;
        if (!EnsureRing()) {
            syslog(LOG_ERR, "ADVCtlAudio: StartIO failed; ring unavailable errno=%d", errno);
            return kAudioHardwareUnspecifiedError;
        }
        if (runningClients_ == 1) {
            ResetRingCursors();
        }
        SetDemand(1);
        syslog(LOG_NOTICE,
               "ADVCtlAudio: StartIO running=%u read=%llu write=%llu underruns=%llu overruns=%llu",
               runningClients_,
               header_->readIndex,
               header_->writeIndex,
               header_->underrunCount,
               header_->overrunCount);
        return kAudioHardwareNoError;
    }

    void OnStopIO() override
    {
        if (runningClients_ > 0) {
            runningClients_ -= 1;
        }
        SetDemand(runningClients_ > 0 ? 1 : 0);
        syslog(LOG_NOTICE,
               "ADVCtlAudio: StopIO running=%u read=%llu write=%llu underruns=%llu overruns=%llu",
               runningClients_,
               header_ ? header_->readIndex : 0,
               header_ ? header_->writeIndex : 0,
               header_ ? header_->underrunCount : 0,
               header_ ? header_->overrunCount : 0);
    }

    std::shared_ptr<aspl::Client> OnAddClient(const aspl::ClientInfo& clientInfo) override
    {
        syslog(LOG_NOTICE,
               "ADVCtlAudio: add client id=%u pid=%d bundle=%s",
               clientInfo.ClientID,
               clientInfo.ProcessID,
               clientInfo.BundleID.c_str());
        return std::make_shared<aspl::Client>(clientInfo);
    }

    void OnRemoveClient(std::shared_ptr<aspl::Client> client) override
    {
        if (client) {
            syslog(LOG_NOTICE,
                   "ADVCtlAudio: remove client id=%u pid=%d",
                   client->GetClientID(),
                   client->GetProcessID());
        }
    }

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override
    {
        (void)client;
        (void)stream;
        (void)zeroTimestamp;
        (void)timestamp;

        auto* out = static_cast<Float32*>(bytes);
        const UInt32 frameCount = bytesCount / sizeof(Float32);
        UInt32 underruns = 0;

        if (EnsureRing()) {
            UInt64 readIndex = header_->readIndex;
            const UInt64 writeIndex = header_->writeIndex;
            for (UInt32 frame = 0; frame < frameCount; ++frame) {
                if (readIndex < writeIndex) {
                    out[frame] = samples_[readIndex % kRingCapacityFrames];
                    readIndex += 1;
                } else {
                    out[frame] = 0.0f;
                    underruns += 1;
                }
            }
            header_->readIndex = readIndex;
            header_->underrunCount += underruns;
        } else {
            std::memset(bytes, 0, bytesCount);
            underruns = frameCount;
        }

        ioCycles_ += 1;
        if (ioCycles_ == 1 || (ioCycles_ % 250) == 0 || (underruns > 0 && (ioCycles_ % 25) == 0)) {
            syslog(LOG_NOTICE,
                   "ADVCtlAudio: ReadInput cycle=%llu frames=%u underruns=%u read=%llu write=%llu demand=%u",
                   ioCycles_,
                   frameCount,
                   underruns,
                   header_ ? header_->readIndex : 0,
                   header_ ? header_->writeIndex : 0,
                   header_ ? header_->demand : 0);
        }
    }

private:
    bool EnsureRing()
    {
        if (header_ != nullptr && samples_ != nullptr) {
            return true;
        }

        mapSize_ = sizeof(RingHeader) + static_cast<size_t>(kRingCapacityFrames) * sizeof(Float32);
        const int fd = open(kRingPath, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            syslog(LOG_ERR, "ADVCtlAudio: open(%s) failed errno=%d", kRingPath, errno);
            return false;
        }
        if (fchmod(fd, 0666) != 0) {
            syslog(LOG_ERR, "ADVCtlAudio: fchmod ring failed errno=%d", errno);
        }
        if (ftruncate(fd, static_cast<off_t>(mapSize_)) != 0) {
            syslog(LOG_ERR, "ADVCtlAudio: ftruncate ring failed errno=%d", errno);
            close(fd);
            return false;
        }

        void* mapped = mmap(nullptr, mapSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) {
            syslog(LOG_ERR, "ADVCtlAudio: mmap ring failed errno=%d", errno);
            mapped_ = nullptr;
            return false;
        }

        mapped_ = mapped;
        header_ = static_cast<RingHeader*>(mapped_);
        samples_ = reinterpret_cast<Float32*>(static_cast<std::uint8_t*>(mapped_) + sizeof(RingHeader));

        if (header_->magic != kRingMagic ||
            header_->version != kRingVersion ||
            header_->capacityFrames != kRingCapacityFrames) {
            std::memset(mapped_, 0, mapSize_);
            header_->magic = kRingMagic;
            header_->version = kRingVersion;
            header_->capacityFrames = kRingCapacityFrames;
        }

        syslog(LOG_NOTICE,
               "ADVCtlAudio: shared ring ready path=%s frames=%u bytes=%zu",
               kRingPath,
               kRingCapacityFrames,
               mapSize_);
        return true;
    }

    void SetDemand(UInt32 demand)
    {
        if (!EnsureRing()) {
            return;
        }
        header_->demand = demand;
        header_->runningClients = runningClients_;
    }

    void ResetRingCursors()
    {
        if (!header_) {
            return;
        }
        header_->readIndex = 0;
        header_->writeIndex = 0;
        header_->underrunCount = 0;
        header_->overrunCount = 0;
    }

    void* mapped_ = nullptr;
    RingHeader* header_ = nullptr;
    Float32* samples_ = nullptr;
    size_t mapSize_ = 0;
    UInt32 runningClients_ = 0;
    UInt64 ioCycles_ = 0;
};

AudioStreamBasicDescription Float32MonoFormat()
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = kChannelCount;
    format.mBytesPerFrame = sizeof(Float32) * kChannelCount;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}

std::shared_ptr<aspl::Driver> CreateDriver()
{
    auto tracer = std::make_shared<aspl::Tracer>(aspl::Tracer::Mode::Noop);
    auto context = std::make_shared<aspl::Context>(tracer);

    aspl::DeviceParameters deviceParams;
    deviceParams.Name = "ADVCtl Audio";
    deviceParams.Manufacturer = "ADVCtl";
    deviceParams.DeviceUID = "com.advctl.audio.device";
    deviceParams.ModelUID = "com.advctl.audio.model";
    deviceParams.ConfigurationApplicationBundleID = "dev.cardputer.advctl";
    deviceParams.CanBeDefault = true;
    deviceParams.CanBeDefaultForSystemSounds = false;
    deviceParams.SampleRate = kSampleRate;
    deviceParams.ChannelCount = kChannelCount;
    deviceParams.ZeroTimeStampPeriod = kSampleRate / 2;
    deviceParams.ClockIsStable = true;
    deviceParams.EnableRealtimeTracing = false;

    auto device = std::make_shared<aspl::Device>(context, deviceParams);

    aspl::StreamParameters streamParams;
    streamParams.Direction = aspl::Direction::Input;
    streamParams.Format = Float32MonoFormat();
    device->AddStreamWithControlsAsync(streamParams);

    auto handler = std::make_shared<ADVCtlHandler>();
    device->SetControlHandler(handler);
    device->SetIOHandler(handler);

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);

    syslog(LOG_NOTICE, "ADVCtlAudio: create driver with libASPL sampleRate=%u channels=%u", kSampleRate, kChannelCount);
    return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace

extern "C" void* ADVCtlAudio_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    (void)allocator;
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
