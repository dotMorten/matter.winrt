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

void TestAllocationRejectionResponses()
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
        Implementation::GenericInvokeState state(2, Camera::Id, Allocation::Id, {}, std::nullopt,
                                                 Camera::Commands::VideoStreamAllocateResponse::Id, true);
        const chip::app::StatusIB rejection(status);
        state.OnResponse(nullptr, chip::app::ConcreteCommandPath(2, Camera::Id, Allocation::Id), rejection, nullptr);
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
    Implementation::GenericInvokeState malformed(2, Camera::Id, Allocation::Id, {}, std::nullopt,
                                                 Camera::Commands::VideoStreamAllocateResponse::Id, true);
    malformed.OnResponse(nullptr, chip::app::ConcreteCommandPath(3, Camera::Id, Allocation::Id),
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
        TestZeroReferenceAllocatedMetadata();
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
