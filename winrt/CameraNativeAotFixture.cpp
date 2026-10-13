#include "ProjectionTypes.h"

#include <clusters/CameraAvStreamManagement/ClusterId.h>
#include <clusters/Descriptor/ClusterId.h>
#include <clusters/WebRTCTransportProvider/ClusterId.h>
#include <clusters/WebRTCTransportProvider/CommandIds.h>
#include <lib/core/CHIPError.h>

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraAudioFixture(int32_t variant, void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        namespace Camera = chip::app::Clusters::CameraAvStreamManagement;
        namespace Collections = winrt::Windows::Foundation::Collections;
        Collections::PropertySet fields;
        for (auto [key, number] : std::initializer_list<std::pair<wchar_t const *, uint64_t>>{
            { L"0", 0 }, { L"1", 3 }, { L"2", 0 }, { L"3", 2 }, { L"4", 48000 },
            { L"5", 64000 }, { L"6", 16 }, { L"7", 0 } })
        {
            fields.Insert(key, winrt::box_value(number));
        }
        auto stream = winrt::make<Implementation::MatterCameraAudioStream>(fields);
        if (variant == 0)
        {
            *value = winrt::detach_abi(stream);
        }
        else if (variant == 1)
        {
            fields.Clear();
            fields.Insert(L"0", winrt::box_value(uint64_t(2)));
            fields.Insert(L"1", winrt::single_threaded_vector<winrt::Windows::Foundation::IInspectable>({
                winrt::box_value(uint64_t(0)), winrt::box_value(uint64_t(255)) }));
            fields.Insert(L"2", winrt::single_threaded_vector<winrt::Windows::Foundation::IInspectable>({
                winrt::box_value(uint64_t(16000)), winrt::box_value(uint64_t(48000)) }));
            fields.Insert(L"3", winrt::single_threaded_vector<winrt::Windows::Foundation::IInspectable>({
                winrt::box_value(uint64_t(16)), winrt::box_value(uint64_t(24)) }));
            auto caps = winrt::make_self<Implementation::MatterCameraAudioCapabilities>();
            caps->mMicrophone = winrt::make<Implementation::MatterCameraAudioFormatCapabilities>(fields);
            caps->mMicrophoneMuted = winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>();
            caps->mMicrophoneVolume = winrt::box_value(uint8_t(20)).as<winrt::Windows::Foundation::IReference<uint8_t>>();
            caps->mBandwidth = 128000;
            *value = winrt::detach_abi(Controller::MatterCameraAudioCapabilities(*caps));
        }
        else if (variant == 2)
        {
            *value = winrt::detach_abi(winrt::make<Implementation::MatterCameraAudioStreamAllocationResult>(stream));
        }
        else if (variant == 3)
        {
            *value = winrt::detach_abi(winrt::make<Implementation::MatterCameraAudioStreamAllocationResult>(
                winrt::make<Implementation::MatterCameraFailure>(Controller::MatterCameraStage::StreamAllocation,
                    int32_t(0x8007001f), true, CHIP_IM_GLOBAL_STATUS(DynamicConstraintError).AsInteger(), L"Audio allocation rejected")));
        }
        else
        {
            throw winrt::hresult_invalid_argument();
        }
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetRetainedCameraSession(void * controllerValue, void ** value) noexcept
{
    if (controllerValue == nullptr || value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        Controller::MatterController controller{ nullptr };
        winrt::copy_from_abi(controller, controllerValue);
        auto runtime = winrt::get_self<Implementation::MatterController>(controller)->Runtime();
        auto session = winrt::make<Implementation::MatterCameraSession>(
            runtime, UINT64_MAX, uint64_t(1), uint16_t(2), uint16_t(3));
        auto video = winrt::make<Implementation::MatterCameraStreamSelection>(Controller::MatterCameraStreamSelectionKind::Absent, nullptr);
        auto audio = winrt::make<Implementation::MatterCameraStreamSelection>(Controller::MatterCameraStreamSelectionKind::AutoSelect, nullptr);
        winrt::get_self<Implementation::MatterCameraSession>(session)->SetMediaMetadata(video, audio, std::nullopt, uint16_t(0), uint16_t(0));
        *value = winrt::detach_abi(session);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraCapabilities(void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        namespace Commands = chip::app::Clusters::WebRTCTransportProvider::Commands;
        auto descriptor = winrt::make<Implementation::MatterEndpointDescriptor>(
            uint16_t(2), std::vector<uint16_t>{},
            std::vector<uint32_t>{ chip::app::Clusters::CameraAvStreamManagement::Id,
                                   chip::app::Clusters::WebRTCTransportProvider::Id },
            std::vector<uint32_t>{}, std::vector<Controller::MatterDeviceType>{});
        auto stream = winrt::make<Implementation::MatterCameraClusterCapabilities>(
            chip::app::Clusters::CameraAvStreamManagement::Id, uint16_t(2), uint32_t(0x80000002),
            std::vector<uint32_t>{ 1, 2 }, std::vector<uint32_t>{}, std::vector<uint32_t>{});
        auto transport = winrt::make<Implementation::MatterCameraClusterCapabilities>(
            chip::app::Clusters::WebRTCTransportProvider::Id, uint16_t(1), uint32_t(0),
            std::vector<uint32_t>{ 0 },
            std::vector<uint32_t>{ Commands::ProvideOffer::Id, Commands::ProvideICECandidates::Id, Commands::EndSession::Id },
            std::vector<uint32_t>{ Commands::ProvideOfferResponse::Id });
        *value = winrt::detach_abi(winrt::make<Implementation::MatterCameraCapabilities>(descriptor, stream, transport));
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraDiscoveryResult(int32_t variant, void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        if (variant < 0 || variant > 2)
        {
            throw winrt::hresult_invalid_argument();
        }
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        Controller::MatterCameraDiscoveryResult result{ nullptr };
        if (variant == 2)
        {
            result = winrt::make<Implementation::MatterCameraDiscoveryResult>(
                winrt::make<Implementation::MatterCameraFailure>(
                    Controller::MatterCameraStage::Discovery, int32_t(0x8007001f), true,
                    CHIP_ERROR_ACCESS_DENIED.AsInteger(), L"Discovery access denied"));
        }
        else
        {
            std::vector<Controller::MatterEndpointDescriptor> cameras;
            if (variant == 1)
            {
                for (uint16_t endpoint : { uint16_t(2), uint16_t(3) })
                {
                    cameras.push_back(winrt::make<Implementation::MatterEndpointDescriptor>(
                        endpoint, std::vector<uint16_t>{},
                        std::vector<uint32_t>{ chip::app::Clusters::CameraAvStreamManagement::Id,
                                               chip::app::Clusters::WebRTCTransportProvider::Id },
                        std::vector<uint32_t>{}, std::vector<Controller::MatterDeviceType>{
                            winrt::make<Implementation::MatterDeviceType>(uint32_t(0x0140), uint16_t(1)) }));
                }
            }
            result = winrt::make<Implementation::MatterCameraDiscoveryResult>(
                winrt::single_threaded_vector(std::move(cameras)).GetView());
        }
        *value = winrt::detach_abi(result);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraOperationResult(int32_t variant, void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        if (variant < 0 || variant > 2)
        {
            throw winrt::hresult_invalid_argument();
        }
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        Controller::MatterCameraOperationResult result{ nullptr };
        if (variant == 0)
        {
            result = winrt::make<Implementation::MatterCameraOperationResult>();
        }
        else
        {
            auto stage = variant == 1 ? Controller::MatterCameraStage::IceCandidates : Controller::MatterCameraStage::SessionEnd;
            result = winrt::make<Implementation::MatterCameraOperationResult>(
                winrt::make<Implementation::MatterCameraFailure>(
                    stage, int32_t(0x8007001f), true, CHIP_ERROR_TIMEOUT.AsInteger(), L"Bounded operation timed out"));
        }
        *value = winrt::detach_abi(result);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraSignal(int32_t variant, void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        if (variant < 0 || variant > 2)
        {
            throw winrt::hresult_invalid_argument();
        }
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        std::optional<uint16_t> protocolId;
        if (variant != 0)
        {
            protocolId = variant == 1 ? uint16_t(0) : uint16_t(42);
        }
        auto signal = winrt::make<Implementation::MatterCameraSignal>(
            uint64_t(5), Controller::MatterCameraSignalKind::Failure, winrt::hstring{},
            std::vector<Controller::MatterCameraIceCandidate>{}, CHIP_ERROR_TIMEOUT.AsInteger(), protocolId, uint8_t(0));
        *value = winrt::detach_abi(signal);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraDescriptor(void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        auto deviceType = winrt::make<Implementation::MatterDeviceType>(uint32_t(0x0140), uint16_t(1));
        auto descriptor = winrt::make<Implementation::MatterEndpointDescriptor>(
            uint16_t(2), std::vector<uint16_t>{ 3, 4 },
            std::vector<uint32_t>{ chip::app::Clusters::Descriptor::Id, chip::app::Clusters::CameraAvStreamManagement::Id,
                                   chip::app::Clusters::WebRTCTransportProvider::Id },
            std::vector<uint32_t>{}, std::vector<Controller::MatterDeviceType>{ deviceType });
        *value = winrt::detach_abi(descriptor);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraVideoStream(void ** value) noexcept
{
    if (value == nullptr)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Collections = winrt::Windows::Foundation::Collections;
        Collections::PropertySet fields;
        fields.Insert(L"0", winrt::box_value(uint64_t(7)));
        fields.Insert(L"1", winrt::box_value(uint64_t(chip::app::Clusters::Globals::StreamUsageEnum::kLiveView)));
        fields.Insert(L"2", winrt::box_value(uint64_t(0)));
        fields.Insert(L"3", winrt::box_value(uint64_t(15)));
        fields.Insert(L"4", winrt::box_value(uint64_t(30)));
        Collections::PropertySet minimum;
        minimum.Insert(L"0", winrt::box_value(uint64_t(640)));
        minimum.Insert(L"1", winrt::box_value(uint64_t(480)));
        Collections::PropertySet maximum;
        maximum.Insert(L"0", winrt::box_value(uint64_t(1920)));
        maximum.Insert(L"1", winrt::box_value(uint64_t(1080)));
        fields.Insert(L"5", minimum);
        fields.Insert(L"6", maximum);
        fields.Insert(L"7", winrt::box_value(uint64_t(100000)));
        fields.Insert(L"8", winrt::box_value(uint64_t(4000000)));
        fields.Insert(L"9", winrt::box_value(uint64_t(2000)));
        fields.Insert(L"10", winrt::box_value(false));
        fields.Insert(L"12", winrt::box_value(uint64_t(0)));
        auto stream = winrt::make<winrt::Matter::Windows::Controller::implementation::MatterCameraVideoStream>(fields);
        *value = winrt::detach_abi(stream);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) int32_t __stdcall GetCameraVideoStreamAllocationResult(int32_t variant, void ** value) noexcept
{
    if (value == nullptr || variant < 0 || variant > 8)
    {
        return winrt::hresult_invalid_argument().code();
    }
    *value = nullptr;
    try
    {
        namespace Implementation = winrt::Matter::Windows::Controller::implementation;
        namespace Controller = winrt::Matter::Windows::Controller;
        Controller::MatterCameraVideoStreamAllocationResult result{ nullptr };
        if (variant == 0)
        {
            void * pointer = nullptr;
            winrt::check_hresult(GetCameraVideoStream(&pointer));
            Controller::MatterCameraVideoStream stream{ nullptr };
            winrt::attach_abi(stream, pointer);
            result = winrt::make<Implementation::MatterCameraVideoStreamAllocationResult>(stream);
        }
        else if (variant == 1)
        {
            result = winrt::make<Implementation::MatterCameraVideoStreamAllocationResult>(
                winrt::make<Implementation::MatterCameraFailure>(Controller::MatterCameraStage::StreamAllocation,
                    int32_t(0x8007001f), true, CHIP_ERROR_TIMEOUT.AsInteger(), L"Allocation outcome is uncertain."));
        }
        else
        {
            const CHIP_ERROR errors[] = {
                CHIP_IM_GLOBAL_STATUS(DynamicConstraintError), CHIP_IM_GLOBAL_STATUS(ConstraintError),
                CHIP_IM_GLOBAL_STATUS(InvalidCommand), CHIP_IM_GLOBAL_STATUS(DynamicConstraintError),
                CHIP_ERROR_INVALID_ARGUMENT, CHIP_ERROR_CONNECTION_ABORTED, CHIP_ERROR_INTERNAL
            };
            auto stage = variant == 5 ? Controller::MatterCameraStage::StreamDeallocation
                                      : Controller::MatterCameraStage::StreamAllocation;
            result = winrt::make<Implementation::MatterCameraVideoStreamAllocationResult>(
                winrt::make<Implementation::MatterCameraFailure>(stage,
                    int32_t(0x8007001f), true, errors[variant - 2].AsInteger(), L"Test camera operation failure."));
        }
        *value = winrt::detach_abi(result);
        return 0;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}
