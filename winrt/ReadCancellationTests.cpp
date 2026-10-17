#include "ProjectionTypes.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <stdexcept>

namespace Implementation = winrt::Matter::Windows::Controller::implementation;
namespace Controller = winrt::Matter::Windows::Controller;

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto storage = std::filesystem::temp_directory_path() /
        ("matter-read-cancellation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Controller::MatterController controller{ nullptr };
    int exitCode = 0;
    try
    {
        auto options = winrt::make<Implementation::ControllerOptions>();
        options.StoragePath(storage.wstring());
        controller = Implementation::MatterController::CreateAsync(options).get();
        auto path = winrt::make<Implementation::AttributePath>(0, 0x0028, 0x0002);
        auto read = controller.ReadAttributeAsync(0x123456789ULL, path);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (read.Status() != winrt::Windows::Foundation::AsyncStatus::Started)
        {
            throw std::runtime_error("The unresolved read did not remain pending; cancellation was not exercised.");
        }
        const auto canceledAt = std::chrono::steady_clock::now();
        read.Cancel();
        auto shutdown = std::async(std::launch::async, [controller] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            controller.CloseAsync().get();
        });
        const bool released = shutdown.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        shutdown.get();
        if (!released)
        {
            throw std::runtime_error("Public read cancellation did not release native work before shutdown.");
        }
        bool wasCanceled = false;
        try
        {
            (void) read.get();
        }
        catch (winrt::hresult_canceled const &)
        {
            wasCanceled = true;
        }
        if (!wasCanceled)
        {
            throw std::runtime_error("The unresolved read did not report cancellation.");
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - canceledAt).count();
        std::cout << "PASS public generic read cancellation released native work and shutdown in " << elapsed << " ms\n";
        controller = nullptr;
        controller = Implementation::MatterController::CreateAsync(options).get();
        controller.CloseAsync().get();
        std::cout << "PASS controller reopened the fresh test storage without pairing\n";
    }
    catch (winrt::hresult_error const & error)
    {
        std::wcerr << L"FAIL " << error.code() << L": " << error.message().c_str() << L'\n';
        exitCode = 1;
    }
    catch (std::exception const & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        exitCode = 1;
    }
    if (controller)
    {
        controller.CloseAsync().get();
        controller = nullptr;
    }
    std::filesystem::remove_all(storage);
    return exitCode;
}
