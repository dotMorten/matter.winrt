// Include the private implementation so these tests use the production TLV bridge without adding DLL exports.
#include "Projection.cpp"

#include <iostream>
#include <stdexcept>

namespace {
namespace Implementation = winrt::Matter::Windows::Controller::implementation;
namespace Camera = chip::app::Clusters::CameraAvStreamManagement;
namespace Allocation = Camera::Commands::VideoStreamAllocate;

void Require(bool condition, char const * message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

Allocation::Type Request(int flags)
{
    Allocation::Type request;
    request.streamUsage = chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;
    request.videoCodec = Camera::VideoCodecEnum::kH264;
    request.minFrameRate = 15;
    request.maxFrameRate = 30;
    request.minResolution.width = 640;
    request.minResolution.height = 480;
    request.maxResolution.width = 1280;
    request.maxResolution.height = 720;
    request.minBitRate = 500000;
    request.maxBitRate = 2000000;
    request.keyFrameInterval = 1000;
    if (flags >= 0)
    {
        request.watermarkEnabled.SetValue(flags != 0);
        request.OSDEnabled.SetValue(flags == 0);
    }
    return request;
}

void VerifyRequest(Allocation::Type const & expected, chip::TLV::TLVReader & reader)
{
    Allocation::DecodableType actual;
    Implementation::CheckChipError(actual.Decode(reader), L"Decode generated allocation round trip");
    Require(actual.streamUsage == expected.streamUsage && actual.videoCodec == expected.videoCodec &&
            actual.minFrameRate == expected.minFrameRate && actual.maxFrameRate == expected.maxFrameRate &&
            actual.minResolution.width == expected.minResolution.width &&
            actual.minResolution.height == expected.minResolution.height &&
            actual.maxResolution.width == expected.maxResolution.width &&
            actual.maxResolution.height == expected.maxResolution.height &&
            actual.minBitRate == expected.minBitRate && actual.maxBitRate == expected.maxBitRate &&
            actual.keyFrameInterval == expected.keyFrameInterval, "Generated allocation fields changed during TLV bridging");
    Require(actual.watermarkEnabled.HasValue() == expected.watermarkEnabled.HasValue() &&
            actual.OSDEnabled.HasValue() == expected.OSDEnabled.HasValue(), "Optional flag presence changed");
    if (expected.watermarkEnabled.HasValue())
    {
        Require(actual.watermarkEnabled.Value() == expected.watermarkEnabled.Value() &&
                actual.OSDEnabled.Value() == expected.OSDEnabled.Value(), "Optional true/false flags changed");
    }
}

void TestAllocationRoundTrip()
{
    for (int flags : { -1, 0, 1 })
    {
        auto request = Request(flags);
        auto arguments = Implementation::CameraCommandArguments(request);
        std::vector<uint8_t> encoded;
        Implementation::CheckChipError(Implementation::EncodePropertySetRoot(arguments, encoded),
                                       L"Encode Matter command arguments");
        Require(arguments.HasKey(L"4") && arguments.HasKey(L"5") && arguments.Size() == (flags < 0 ? 9u : 11u),
                "Nested allocation fields lost their original context tags");
        chip::TLV::TLVReader reader;
        reader.Init(encoded.data(), encoded.size());
        Implementation::CheckChipError(reader.Next(), L"Read re-encoded allocation");
        VerifyRequest(request, reader);
    }
    std::cout << "PASS generated camera allocation TLV round trips with distinct resolutions and absent/true/false flags\n";
}

void TestNestedStructuresInArray()
{
    uint8_t original[1024];
    chip::TLV::TLVWriter writer;
    writer.Init(original, sizeof(original));
    chip::TLV::TLVType root, array;
    Implementation::CheckChipError(writer.StartContainer(chip::TLV::AnonymousTag(), chip::TLV::kTLVType_Structure, root),
                                   L"Start array envelope");
    Implementation::CheckChipError(writer.StartContainer(chip::TLV::ContextTag(12), chip::TLV::kTLVType_Array, array),
                                   L"Start nested structure array");
    for (int flags : { -1, 0, 1 })
    {
        Implementation::CheckChipError(Request(flags).Encode(writer, chip::TLV::AnonymousTag()), L"Encode nested request");
    }
    Implementation::CheckChipError(writer.EndContainer(array), L"End array");
    Implementation::CheckChipError(writer.Put(chip::TLV::ContextTag(13), uint32_t(42)), L"Encode following sibling");
    Implementation::CheckChipError(writer.EndContainer(root), L"End envelope");
    Implementation::CheckChipError(writer.Finalize(), L"Finalize array envelope");
    chip::TLV::TLVReader reader;
    reader.Init(original, writer.GetLengthWritten());
    Implementation::CheckChipError(reader.Next(), L"Read array envelope");
    auto properties = Implementation::DecodePropertySetRoot(reader);
    Require(properties.HasKey(L"12") && properties.HasKey(L"13") && properties.Size() == 2,
            "Array field or following sibling lost its context tag");
    std::vector<uint8_t> encoded;
    Implementation::CheckChipError(Implementation::EncodePropertySetRoot(properties, encoded), L"Re-encode array envelope");
    reader.Init(encoded.data(), encoded.size());
    Implementation::CheckChipError(reader.Next(), L"Read re-encoded envelope");
    Implementation::CheckChipError(reader.EnterContainer(root), L"Enter envelope");
    Implementation::CheckChipError(reader.Next(), L"Read array");
    Require(reader.GetTag() == chip::TLV::ContextTag(12), "Array context tag changed");
    Implementation::CheckChipError(reader.EnterContainer(array), L"Enter array");
    for (int flags : { -1, 0, 1 })
    {
        Implementation::CheckChipError(reader.Next(), L"Read nested request");
        Require(reader.GetTag() == chip::TLV::AnonymousTag(), "Array item became context tagged");
        VerifyRequest(Request(flags), reader);
    }
    Require(reader.Next() == CHIP_END_OF_TLV, "Array item count changed");
    Implementation::CheckChipError(reader.ExitContainer(array), L"Exit array");
    Implementation::CheckChipError(reader.Next(), L"Read following sibling");
    uint32_t sibling;
    Implementation::CheckChipError(reader.Get(sibling), L"Read sibling value");
    Require(reader.GetTag() == chip::TLV::ContextTag(13) && sibling == 42, "Following sibling changed");
    Require(reader.Next() == CHIP_END_OF_TLV, "Envelope field count changed");
    Implementation::CheckChipError(reader.ExitContainer(root), L"Exit envelope");
    std::cout << "PASS structures with nested resolutions inside arrays and following sibling fields\n";
}

void TestPreDispatchFailures()
{
    namespace Controller = winrt::Matter::Windows::Controller;
    auto storage = std::filesystem::temp_directory_path() /
        ("matter-tlv-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto cleanup = chip::ScopeExit([&] { std::filesystem::remove_all(storage); });
    auto options = winrt::make<Implementation::ControllerOptions>();
    options.StoragePath(storage.wstring());
    auto runtime = Implementation::ControllerRuntime::Create(options);
    auto close = chip::ScopeExit([&] { runtime->Close(); });
    runtime->Close();
    winrt::Windows::Foundation::Collections::PropertySet malformed;
    malformed.Insert(L"256", winrt::box_value(uint64_t(1)));
    auto stage = Controller::MatterCameraStage::StreamAllocation;
    auto result = Implementation::RunCameraOperation(stage, [&] {
        runtime->InvokeCommand(1, 2, Camera::Id, Allocation::Id, malformed, std::nullopt,
                               Camera::Commands::VideoStreamAllocateResponse::Id, true, &stage);
    });
    Require(!result.Succeeded() && result.Failure().Stage() == Controller::MatterCameraStage::Validation &&
            result.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidInput &&
            result.Failure().HasNativeError() && result.Failure().NativeError() == CHIP_ERROR_INVALID_ARGUMENT.AsInteger(),
            "A proven local encoder failure was reported as uncertain remote allocation");
    stage = Controller::MatterCameraStage::StreamAllocation;
    result = Implementation::RunCameraOperation(stage, [&] {
        runtime->InvokeCommand(1, 2, Camera::Id, Allocation::Id, Implementation::CameraCommandArguments(Request(-1)),
                               std::nullopt, Camera::Commands::VideoStreamAllocateResponse::Id, true, &stage);
    });
    Require(!result.Succeeded() && result.Failure().Stage() == Controller::MatterCameraStage::Validation &&
            result.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidState,
            "Closed-controller validation was reported as a dispatched allocation");
    std::cout << "PASS local encoding and closed-controller failures remain pre-dispatch validation failures\n";
}

void TestInvalidStructureTag()
{
    uint8_t encoded[32];
    chip::TLV::TLVWriter writer;
    writer.Init(encoded, sizeof(encoded));
    chip::TLV::TLVType container;
    Implementation::CheckChipError(writer.StartContainer(chip::TLV::AnonymousTag(), chip::TLV::kTLVType_Array, container),
                                   L"Start anonymous-item container");
    Implementation::CheckChipError(writer.Put(chip::TLV::AnonymousTag(), uint32_t(1)), L"Write anonymous item");
    Implementation::CheckChipError(writer.EndContainer(container), L"End anonymous-item container");
    Implementation::CheckChipError(writer.Finalize(), L"Finalize anonymous-item container");
    // The writer refuses anonymous structure members, so change the anonymous root array into a malformed structure.
    encoded[0] = static_cast<uint8_t>(chip::TLV::kTLVType_Structure);
    chip::TLV::TLVReader reader;
    reader.Init(encoded, writer.GetLengthWritten());
    Implementation::CheckChipError(reader.Next(), L"Read malformed structure");
    winrt::Windows::Foundation::IInspectable value;
    Require(Implementation::DecodeTlvValue(reader, value) == CHIP_ERROR_INVALID_TLV_TAG,
            "Preserving field tags relaxed structure tag validation");
    namespace Controller = winrt::Matter::Windows::Controller;
    auto stage = Controller::MatterCameraStage::StreamAllocation;
    auto uncertain = Implementation::RunCameraOperation(stage, [] {
        Implementation::ThrowChipError(CHIP_ERROR_TIMEOUT, L"Dispatched allocation outcome unknown");
    });
    Require(!uncertain.Succeeded() && uncertain.Failure().Stage() == Controller::MatterCameraStage::StreamAllocation &&
            uncertain.Failure().Category() == Controller::MatterCameraFailureCategory::Timeout &&
            uncertain.Failure().HasNativeError(), "An unknown dispatched allocation lost its uncertainty diagnostics");
    std::cout << "PASS invalid structure tags rejected and dispatched allocation timeout diagnostics preserved\n";
}

void TestAllocationRejectionResponses(chip::CommandId command = Allocation::Id,
                                     chip::CommandId response = Camera::Commands::VideoStreamAllocateResponse::Id)
{
    namespace Controller = winrt::Matter::Windows::Controller;
    using Category = Controller::MatterCameraFailureCategory;
    using Status = chip::Protocols::InteractionModel::Status;
    for (auto [status, category] : {
             std::pair{ Status::DynamicConstraintError, Category::DynamicConstraintError },
             std::pair{ Status::ConstraintError, Category::ConstraintError },
             std::pair{ Status::InvalidCommand, Category::CommandRejected },
             std::pair{ Status::Failure, Category::CommandRejected } })
    {
        Implementation::GenericInvokeState state(2, Camera::Id, command, {}, std::nullopt, response, true);
        const chip::app::StatusIB rejection(status);
        state.OnResponse(nullptr, chip::app::ConcreteCommandPath(2, Camera::Id, command), rejection, nullptr);
        state.OnDone(nullptr);
        Require(state.complete && state.result == rejection.ToChipError() && !state.value,
                "A camera rejection supplied allocation metadata or lost its exact status");
        auto stage = Controller::MatterCameraStage::StreamAllocation;
        auto result = Implementation::RunCameraOperation(stage, [&] {
            Implementation::CheckChipError(state.result, L"Invoke Matter command");
        });
        Require(!result.Succeeded() && result.Failure().Category() == category &&
                result.Failure().Stage() == Controller::MatterCameraStage::StreamAllocation &&
                result.Failure().HasNativeError() && result.Failure().NativeError() == rejection.ToChipError().AsInteger() &&
                !result.Failure().Retryable(), "Explicit allocation rejection became unknown/uncertain or retryable");
    }
    Implementation::GenericInvokeState malformed(2, Camera::Id, command, {}, std::nullopt, response, true);
    malformed.OnResponse(nullptr, chip::app::ConcreteCommandPath(3, Camera::Id, command),
                         chip::app::StatusIB(Status::DynamicConstraintError), nullptr);
    malformed.OnDone(nullptr);
    Require(malformed.result == CHIP_ERROR_INVALID_ARGUMENT, "An uncorrelated rejection was treated as a known outcome");
    std::cout << "PASS correlated allocation rejection statuses and uncorrelated-response ownership safeguards\n";
}

void TestZeroReferenceAllocatedMetadata()
{
    auto options = winrt::make<Implementation::MatterCameraVideoStreamOptions>();
    auto defaults = winrt::get_self<Implementation::MatterCameraVideoStreamOptions>(options)->Snapshot();
    options.MinimumWidth(1920);
    options.MaximumWidth(1920);
    options.MinimumHeight(1080);
    options.MaximumHeight(1080);
    options.MinimumFrameRate(30);
    options.MaximumFrameRate(120);
    options.MinimumBitRate(10000);
    options.MaximumBitRate(10000);
    options.KeyFrameInterval(4000);
    auto g350 = winrt::get_self<Implementation::MatterCameraVideoStreamOptions>(options)->Snapshot();
    for (auto const & request : { defaults, g350 })
    {
        for (uint8_t count : { uint8_t(0), uint8_t(1), UINT8_MAX })
        {
            Camera::Structs::VideoStreamStruct::Type metadata;
            metadata.videoStreamID = 3;
            metadata.streamUsage = request.streamUsage;
            metadata.videoCodec = request.videoCodec;
            metadata.minResolution = request.minResolution;
            metadata.maxResolution = request.maxResolution;
            metadata.minFrameRate = request.minFrameRate;
            metadata.maxFrameRate = request.maxFrameRate;
            metadata.minBitRate = request.minBitRate;
            metadata.maxBitRate = request.maxBitRate;
            metadata.keyFrameInterval = request.keyFrameInterval;
            metadata.referenceCount = count;
            auto stream = winrt::make<Implementation::MatterCameraVideoStream>(Implementation::CameraCommandArguments(metadata));
            Implementation::MatterCameraVideoStreamOptions::ValidateAllocated(request, stream);
            Require(stream.ReferenceCount() == count, "Transport reference count was coerced instead of preserved");
            auto streams = winrt::single_threaded_vector<winrt::Matter::Windows::Controller::MatterCameraVideoStream>(
                { stream }).GetView();
            Require(Implementation::FindAllocatedLiveViewStream(streams, 3) == stream,
                    "Allocation/start selection rejected an unused LiveView stream");
            Require(!Implementation::FindAllocatedLiveViewStream(streams, 4),
                    "Selection accepted a stream other than the returned ID");
            metadata.streamUsage = chip::app::Clusters::Globals::StreamUsageEnum::kRecording;
            auto recording = winrt::make<Implementation::MatterCameraVideoStream>(
                Implementation::CameraCommandArguments(metadata));
            auto otherUsage = winrt::single_threaded_vector<winrt::Matter::Windows::Controller::MatterCameraVideoStream>(
                { recording }).GetView();
            Require(!Implementation::FindAllocatedLiveViewStream(otherUsage, 3),
                    "Zero reference count permitted a non-LiveView stream");
        }
    }
    std::cout << "PASS default and reported G350 allocation metadata with zero/nonzero transport references\n";
}

template <class Operation>
void RequireHResult(Operation operation, winrt::hresult expected, char const * message)
{
    try
    {
        operation();
    }
    catch (winrt::hresult_error const & error)
    {
        Require(error.code() == expected, message);
        return;
    }
    Require(false, message);
}

Camera::Commands::AudioStreamAllocate::Type AudioRequest()
{
    auto options = winrt::make<Implementation::MatterCameraAudioStreamOptions>();
    options.AudioCodec(0);
    options.Channels(2);
    options.SampleRate(48000);
    options.BitRate(64000);
    options.BitDepth(16);
    return winrt::get_self<Implementation::MatterCameraAudioStreamOptions>(options)->Snapshot();
}

winrt::Matter::Windows::Controller::MatterCameraAudioFormatCapabilities AudioFormats(
    std::initializer_list<uint8_t> depths = { 16, 24 })
{
    Camera::Structs::AudioCapabilitiesStruct::Type capabilities;
    Camera::AudioCodecEnum codecs[]{ Camera::AudioCodecEnum::kOpus, Camera::AudioCodecEnum::kAacLc,
                                   static_cast<Camera::AudioCodecEnum>(255) };
    uint32_t rates[]{ 16000, 48000 };
    capabilities.maxNumberOfChannels = 2;
    capabilities.supportedCodecs = chip::app::DataModel::List<const Camera::AudioCodecEnum>(codecs);
    capabilities.supportedSampleRates = chip::app::DataModel::List<const uint32_t>(rates);
    capabilities.supportedBitDepths = chip::app::DataModel::List<const uint8_t>(depths.begin(), depths.size());
    return winrt::make<Implementation::MatterCameraAudioFormatCapabilities>(
        Implementation::CameraCommandArguments(capabilities));
}

winrt::Matter::Windows::Controller::MatterCameraCapabilities AudioCamera(
    uint32_t deviceType = 0x0142, uint16_t revision = 2, bool requestor = true, bool commands = true)
{
    namespace Controller = winrt::Matter::Windows::Controller;
    namespace Provider = chip::app::Clusters::WebRTCTransportProvider;
    auto descriptor = winrt::make<Implementation::MatterEndpointDescriptor>(
        uint16_t(2), std::vector<uint16_t>{},
        std::vector<uint32_t>{ Camera::Id, Provider::Id },
        requestor ? std::vector<uint32_t>{ chip::app::Clusters::WebRTCTransportRequestor::Id } : std::vector<uint32_t>{},
        std::vector<Controller::MatterDeviceType>{
            winrt::make<Implementation::MatterDeviceType>(deviceType, uint16_t(2)) });
    auto stream = winrt::make<Implementation::MatterCameraClusterCapabilities>(
        Camera::Id, revision, uint32_t(Camera::Feature::kAudio) | uint32_t(Camera::Feature::kSpeaker),
        std::vector<uint32_t>{ Camera::Attributes::MicrophoneCapabilities::Id, Camera::Attributes::AllocatedAudioStreams::Id,
                               Camera::Attributes::SupportedStreamUsages::Id },
        commands ? std::vector<uint32_t>{ Camera::Commands::AudioStreamAllocate::Id, Camera::Commands::AudioStreamDeallocate::Id }
                 : std::vector<uint32_t>{},
        std::vector<uint32_t>{ Camera::Commands::AudioStreamAllocateResponse::Id });
    auto provider = winrt::make<Implementation::MatterCameraClusterCapabilities>(
        Provider::Id, uint16_t(2), uint32_t(0), std::vector<uint32_t>{},
        std::vector<uint32_t>{ Provider::Commands::ProvideOffer::Id, Provider::Commands::ProvideICECandidates::Id,
                               Provider::Commands::EndSession::Id },
        std::vector<uint32_t>{ Provider::Commands::ProvideOfferResponse::Id });
    return winrt::make<Implementation::MatterCameraCapabilities>(descriptor, stream, provider);
}

void TestAudioTlvAndMetadata()
{
    auto options = winrt::make<Implementation::MatterCameraAudioStreamOptions>();
    RequireHResult([&] { winrt::get_self<Implementation::MatterCameraAudioStreamOptions>(options)->Snapshot(); },
                   E_INVALIDARG, "Audio defaults silently chose a format");
    auto request = AudioRequest();
    std::vector<uint8_t> encoded;
    Implementation::CheckChipError(Implementation::EncodePropertySetRoot(
        Implementation::CameraCommandArguments(request), encoded), L"Re-encode audio allocation");
    chip::TLV::TLVReader reader;
    reader.Init(encoded.data(), encoded.size());
    Implementation::CheckChipError(reader.Next(), L"Read audio allocation");
    Camera::Commands::AudioStreamAllocate::DecodableType decoded;
    Implementation::CheckChipError(decoded.Decode(reader), L"Decode audio allocation");
    Require(decoded.streamUsage == request.streamUsage && decoded.audioCodec == request.audioCodec &&
            decoded.channelCount == 2 && decoded.sampleRate == 48000 && decoded.bitRate == 64000 && decoded.bitDepth == 16,
            "Generated audio allocation fields changed in the production TLV bridge");
    auto formats = AudioFormats();
    Require(formats.MaximumChannels() == 2 && formats.Codecs().Size() == 3 && formats.Codecs().GetAt(2) == 255 &&
            formats.SampleRates().GetAt(1) == 48000 && formats.BitDepths().GetAt(1) == 24,
            "Nested capability lists or unknown codec values were lost");
    auto format = winrt::get_self<Implementation::MatterCameraAudioFormatCapabilities>(formats);
    format->Validate(request);
    request.channelCount = 3;
    RequireHResult([&] { format->Validate(request); }, E_NOTIMPL, "Unadvertised channels were accepted");
    request = AudioRequest();
    request.sampleRate = 44100;
    RequireHResult([&] { format->Validate(request); }, E_NOTIMPL, "Unadvertised rate was accepted");
    request = AudioRequest();
    Camera::Structs::AudioStreamStruct::Type metadata;
    metadata.audioStreamID = 0;
    metadata.streamUsage = request.streamUsage;
    metadata.audioCodec = request.audioCodec;
    metadata.channelCount = request.channelCount;
    metadata.sampleRate = request.sampleRate;
    metadata.bitRate = request.bitRate;
    metadata.bitDepth = request.bitDepth;
    for (uint8_t count : { uint8_t(0), uint8_t(1), UINT8_MAX })
    {
        metadata.referenceCount = count;
        auto stream = winrt::make<Implementation::MatterCameraAudioStream>(Implementation::CameraCommandArguments(metadata));
        Implementation::MatterCameraAudioStreamOptions::ValidateAllocated(request, stream);
        Require(stream.StreamId() == 0 && stream.ReferenceCount() == count, "Audio ID zero or transport count was coerced");
        auto result = winrt::make<Implementation::MatterCameraAudioStreamAllocationResult>(stream);
        Require(result.Succeeded() && !result.Failure(), "Audio result did not preserve success discrimination");
    }
    for (int field = 0; field < 6; ++field)
    {
        auto fields = Implementation::CameraCommandArguments(metadata);
        fields.Insert(std::to_wstring(field + 1), winrt::box_value(uint64_t(
            field == 0 ? uint64_t(chip::app::Clusters::Globals::StreamUsageEnum::kRecording) : uint64_t(1))));
        auto stream = winrt::make<Implementation::MatterCameraAudioStream>(fields);
        RequireHResult([&] { Implementation::MatterCameraAudioStreamOptions::ValidateAllocated(request, stream); },
                       E_BOUNDS, "Mismatched audio metadata was accepted");
    }
    auto fields = Implementation::CameraCommandArguments(metadata);
    fields.Insert(L"2", winrt::box_value(uint64_t(255)));
    Require(winrt::make<Implementation::MatterCameraAudioStream>(fields).AudioCodec() == 255,
            "Unknown allocated audio codec was coerced");
    fields.Insert(L"0", winrt::box_value(uint64_t(UINT16_MAX)));
    RequireHResult([&] { winrt::make<Implementation::MatterCameraAudioStream>(fields); }, E_BOUNDS,
                   "Reserved audio ID was accepted");
    fields.Insert(L"0", winrt::box_value(uint64_t(0)));
    fields.Insert(L"4", winrt::box_value(uint64_t(UINT32_MAX) + 1));
    RequireHResult([&] { winrt::make<Implementation::MatterCameraAudioStream>(fields); }, E_BOUNDS,
                   "Audio sample-rate overflow was narrowed");
    fields = Implementation::CameraCommandArguments(metadata);
    fields.Remove(L"7");
    RequireHResult([&] { winrt::make<Implementation::MatterCameraAudioStream>(fields); }, E_BOUNDS,
                   "Missing audio transport count was defaulted");
    for (auto const & value : { winrt::box_value(int64_t(2)), winrt::box_value(2.0), winrt::box_value(winrt::hstring(L"2")) })
    {
        fields = Implementation::CameraCommandArguments(metadata);
        fields.Insert(L"3", value);
        bool rejected = false;
        try
        {
            winrt::make<Implementation::MatterCameraAudioStream>(fields);
        }
        catch (winrt::hresult_error const &)
        {
            rejected = true;
        }
        Require(rejected, "Signed, floating-point or text audio fields were coerced into unsigned protocol integers");
    }
    std::cout << "PASS generated audio TLV/capability lists, raw codecs, zero transport counts, and exact format validation\n";
}

void TestAudioAdapterAndControls()
{
    auto capabilities = winrt::make_self<Implementation::MatterCameraAudioCapabilities>();
    capabilities->mCamera = AudioCamera();
    capabilities->mMicrophone = AudioFormats();
    Require(capabilities->SupportsListening(), "A compatible camera microphone was rejected");
    for (auto depths : { std::initializer_list<uint8_t>{ 24, 0 }, std::initializer_list<uint8_t>{ 24 },
                         std::initializer_list<uint8_t>{ 0 }, std::initializer_list<uint8_t>{} })
    {
        capabilities->mMicrophone = AudioFormats(depths);
        capabilities->mSpeaker = AudioFormats({ 0 });
        auto raw = capabilities->mMicrophone.BitDepths();
        Require(raw.Size() == depths.size() && std::equal(raw.begin(), raw.end(), depths.begin()),
                "Advertised invalid bit depths were discarded or replaced");
        Require(capabilities->SupportsListening() == (depths.size() != 0 && *depths.begin() == 24),
                "All-invalid microphone or independent invalid speaker eligibility changed");
        auto request = AudioRequest();
        request.bitDepth = 24;
        auto format = winrt::get_self<Implementation::MatterCameraAudioFormatCapabilities>(capabilities->mMicrophone);
        if (capabilities->SupportsListening())
        {
            format->Validate(request);
        }
        else
        {
            RequireHResult([&] { format->Validate(request); }, E_NOTIMPL, "All-invalid depths permitted allocation");
        }
        request.bitDepth = 0;
        RequireHResult([&] { format->Validate(request); }, E_NOTIMPL, "Zero bit depth was selectable");
    }
    capabilities->mMicrophone = AudioFormats();
    for (auto camera : { AudioCamera(0x0143), AudioCamera(0x1234), AudioCamera(0x0142, 3),
                         AudioCamera(0x0142, 2, false), AudioCamera(0x0142, 2, true, false) })
    {
        capabilities->mCamera = camera;
        Require(!capabilities->SupportsListening(), "Non-camera, incompatible revision/composition or missing commands enabled audio");
    }
    capabilities->mCamera = AudioCamera();
    capabilities->mBandwidth = 128000;
    capabilities->mMicrophoneMuted = winrt::box_value(true).as<winrt::Windows::Foundation::IReference<bool>>();
    auto volume = [](uint8_t value) { return winrt::box_value(value).as<winrt::Windows::Foundation::IReference<uint8_t>>(); };
    capabilities->mMicrophoneVolume = volume(20);
    capabilities->mMicrophoneMin = volume(10);
    capabilities->mMicrophoneMax = volume(40);
    auto arguments = Implementation::CameraAudioControlArguments(*capabilities, false, true, 30);
    Require(arguments.first == Camera::Attributes::MicrophoneVolumeLevel::Id && arguments.second.Size() == 1 &&
            winrt::unbox_value<uint8_t>(arguments.second.Lookup(L"value")) == 30, "Volume write changed remote mute or privacy");
    RequireHResult([&] { Implementation::CameraAudioControlArguments(*capabilities, false, true, 41); },
                   E_INVALIDARG, "Volume outside advertised bounds was accepted");
    capabilities->mMicrophoneMin = volume(50);
    RequireHResult([&] { Implementation::CameraAudioControlArguments(*capabilities, false, true, 30); },
                   E_BOUNDS, "Reversed device volume bounds were accepted");
    RequireHResult([&] { Implementation::CameraAudioControlArguments(*capabilities, true, false, 1); },
                   E_NOTIMPL, "Missing speaker capability was defaulted");
    capabilities->mPrivacy = true;
    RequireHResult([&] { Implementation::CameraAudioControlArguments(*capabilities, false, false, 0); },
                   E_ACCESSDENIED, "Privacy was bypassed by unmuting");
    arguments = Implementation::CameraAudioControlArguments(*capabilities, false, false, 1);
    Require(arguments.first == Camera::Attributes::MicrophoneMuted::Id &&
            winrt::unbox_value<bool>(arguments.second.Lookup(L"value")), "Privacy prevented safe explicit mute");
    auto stage = winrt::Matter::Windows::Controller::MatterCameraStage::Capabilities;
    RequireHResult([&] { Implementation::RequireListening(*capabilities, stage); }, E_ACCESSDENIED,
                   "Privacy enabled streaming");
    auto request = AudioRequest();
    Camera::Structs::AudioStreamStruct::Type metadata;
    metadata.audioStreamID = 7;
    metadata.streamUsage = request.streamUsage;
    metadata.audioCodec = request.audioCodec;
    metadata.channelCount = request.channelCount;
    metadata.sampleRate = request.sampleRate;
    metadata.bitRate = request.bitRate;
    metadata.bitDepth = request.bitDepth;
    metadata.referenceCount = 0;
    auto streams = winrt::single_threaded_vector<winrt::Matter::Windows::Controller::MatterCameraAudioStream>({
        winrt::make<Implementation::MatterCameraAudioStream>(Implementation::CameraCommandArguments(metadata)) }).GetView();
    Implementation::ValidateCameraSelectedAudio(*capabilities, streams, 7);
    RequireHResult([&] { Implementation::ValidateCameraSelectedAudio(*capabilities, streams, 8); },
                   E_INVALIDARG, "Audio readback acquired a different ID");
    std::cout << "PASS camera-only revision/command gate, external exact-ID selection and independent remote controls/privacy\n";
}

void TestAudioOwnership()
{
    Implementation::CameraAudioAllocationOwnership ownership;
    const auto invalid = winrt::hresult_illegal_method_call().code();
    RequireHResult([&] { ownership.StartRelease(1, 2, 7); }, invalid, "External audio stream was deallocated");
    ownership.Reserve(1, 2);
    RequireHResult([&] { ownership.Reserve(1, 2); }, invalid, "Concurrent audio allocation was accepted");
    ownership.AllocationFailed(1, 2, true);
    ownership.Reserve(1, 2);
    ownership.Commit(1, 2, 7);
    RequireHResult([&] { ownership.StartRelease(1, 3, 7); }, invalid, "Endpoint ownership was ignored");
    RequireHResult([&] { ownership.StartRelease(2, 2, 7); }, invalid, "Peer ownership was ignored");
    RequireHResult([&] { ownership.StartRelease(1, 2, 8); }, invalid, "Unowned audio ID was released");
    ownership.StartRelease(1, 2, 7);
    RequireHResult([&] { ownership.StartRelease(1, 2, 7); }, invalid, "Duplicate deallocation was dispatched");
    ownership.ReleaseFinished(1, 2, false);
    RequireHResult([&] { ownership.Reserve(1, 2); }, invalid, "Uncertain cleanup permitted a new allocation");
    RequireHResult([&] { ownership.StartRelease(1, 2, 7); }, invalid, "Uncertain cleanup retried a possibly reused ID");
    ownership.Reserve(2, 2);
    ownership.AllocationFailed(2, 2, false);
    RequireHResult([&] { ownership.Reserve(2, 2); }, invalid, "Unknown allocation outcome permitted a blind retry");
    ownership.Reserve(3, 2);
    ownership.Commit(3, 2, 0);
    uint64_t released = 0;
    auto pending = std::make_shared<Implementation::PendingCameraSession>([&](uint64_t handle) {
        Require(released == 0, "Late session cancellation released twice");
        released = handle;
    });
    pending->Cancel();
    pending->Record(UINT64_MAX);
    pending->Release();
    pending->Release();
    Require(released == UINT64_MAX, "Session cancellation narrowed a private 64-bit handle");
    ownership.StartRelease(3, 2, 0);
    ownership.ReleaseFinished(3, 2, true);
    ownership.Reserve(3, 2);
    ownership.Commit(3, 2, 0);
    std::cout << "PASS audio ownership: known refusal, unknown outcome, duplicate release, peer/endpoint checks, and reused zero ID\n";
}

void TestTypedMediaSelections()
{
    namespace Controller = winrt::Matter::Windows::Controller;
    for (int kind = 0; kind <= 3; ++kind)
    {
        for (int value : { -1, 0, int(UINT16_MAX) })
        {
            winrt::Windows::Foundation::IReference<uint16_t> id{ nullptr };
            if (value >= 0)
            {
                id = winrt::box_value(uint16_t(value)).as<winrt::Windows::Foundation::IReference<uint16_t>>();
            }
            auto selection = [&] {
                return winrt::make<Implementation::MatterCameraStreamSelection>(
                    static_cast<Controller::MatterCameraStreamSelectionKind>(kind), id);
            };
            if ((kind <= 1 && value == -1) || (kind == 2 && value == 0))
            {
                auto result = selection();
                Require(int(result.Kind()) == kind && bool(result.StreamId()) == (value >= 0),
                        "Immutable selection lost kind or ID presence");
            }
            else
            {
                RequireHResult(selection, E_INVALIDARG, "An invalid selection kind/nullable ID combination was accepted");
            }
        }
    }
    std::cout << "PASS typed Absent/AutoSelect/ExplicitId constructors, zero IDs and invalid/reserved combinations\n";
}
} // namespace

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        TestAllocationRoundTrip();
        TestNestedStructuresInArray();
        TestPreDispatchFailures();
        TestInvalidStructureTag();
        TestAllocationRejectionResponses();
        TestAllocationRejectionResponses(Camera::Commands::AudioStreamAllocate::Id, Camera::Commands::AudioStreamAllocateResponse::Id);
        TestZeroReferenceAllocatedMetadata();
        TestAudioTlvAndMetadata();
        TestAudioAdapterAndControls();
        TestAudioOwnership();
        TestTypedMediaSelections();
    }
    catch (winrt::hresult_error const & error)
    {
        std::wcerr << L"FAIL HRESULT " << error.code() << L": " << error.message().c_str() << L'\n';
        return 1;
    }
    catch (std::exception const & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
