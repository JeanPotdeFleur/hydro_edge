#include <iostream>
#include <thread>
#include <pthread.h>
#include <memory>
#include <fstream>
#include <string>
#include <csignal>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdint.h>
#include <sys/timepps.h>
#include <chrono>

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "RingBuffer.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

std::atomic<bool> keep_running{true};

void sigint_handler(int signum) {
    std::cout << "\n[SYSTEM] Signal SIGINT (" << signum << ") intercepté. Amorçage du Graceful Shutdown..." << std::endl;
    keep_running = false;
}

// --- THREAD 2 : CONSUMER ---
void consumer_thread(std::shared_ptr<RingBuffer> buffer) {
    pthread_t thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[CRITIQUE] Échec d'allocation d'affinité sur le Cœur 3." << std::endl;
    }

    StereoFrame current_frame;
    std::cout << "[THREAD 2] Consumer actif sur le Cœur 3. Prêt pour les I/O POSIX..." << std::endl;
    int frame_count = 0;

    while (buffer->pop(current_frame)) {
        std::string file0 = "/mnt/vault/cam0_" + std::to_string(current_frame.timestamp) + ".raw";
        std::string file1 = "/mnt/vault/cam1_" + std::to_string(current_frame.timestamp) + ".raw";

        std::ofstream out0(file0, std::ios::binary);
        if (out0) {
            out0.write(reinterpret_cast<const char*>(current_frame.cam0_data.data()), current_frame.cam0_data.size());
            out0.close();
        }
        std::ofstream out1(file1, std::ios::binary);
        if (out1) {
            out1.write(reinterpret_cast<const char*>(current_frame.cam1_data.data()), current_frame.cam1_data.size());
            out1.close();
        }

        frame_count++;
        std::cout << "[I/O] Frame " << frame_count << " déchargée sur le stockage (SSD). TS: " << current_frame.timestamp << " ns" << std::endl;
    }
    
    std::cout << "[THREAD 2] RAM purgée intégralement. Consumer arrêté proprement." << std::endl;
}

// --- THREAD 1 : PRODUCER (Disciplined 2 Hz Loop via 1 Hz PPS Anchor) ---
void producer_thread(SystemPtr system, std::shared_ptr<RingBuffer> buffer) {
    pthread_t thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[CRITIQUE] Échec d'allocation d'affinité sur le Cœur 2." << std::endl;
    }

    std::cout << "[THREAD 1] Producer actif sur le Cœur 2 (Mode Disciplined Hybrid 2Hz)." << std::endl;

    int pps_fd = open("/dev/pps0", O_RDWR);
    if (pps_fd < 0) {
        std::cerr << "[CRITIQUE] Impossible d'ouvrir le buffer noyau /dev/pps0." << std::endl;
        buffer->shutdown();
        return;
    }

    pps_handle_t pps_handle;
    if (time_pps_create(pps_fd, &pps_handle) < 0) {
        std::cerr << "[CRITIQUE] Échec de l'instanciation de l'API PPS." << std::endl;
        close(pps_fd);
        buffer->shutdown();
        return;
    }

    {
        CameraList camList = system->GetCameras();
        
        if (camList.GetSize() < 2) {
            std::cerr << "[CRITIQUE] Topologie incomplète." << std::endl;
            camList.Clear();
            buffer->shutdown();
            return;
        }

        CameraPtr cam0 = camList.GetByIndex(0);
        CameraPtr cam1 = camList.GetByIndex(1);

        try {
            cam0->Init();
            cam1->Init();

            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam0->TriggerSource.SetValue(TriggerSourceEnums::TriggerSource_Software);
            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_On);

            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam1->TriggerSource.SetValue(TriggerSourceEnums::TriggerSource_Software);
            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_On);

            cam0->BeginAcquisition();
            cam1->BeginAcquisition();

            std::cout << "[THREAD 1] Boucle d'acquisition hybride amorcée..." << std::endl;

            pps_info_t info;
            struct timespec timeout = {2, 0};

            while (keep_running) {
                if (time_pps_fetch(pps_handle, PPS_TSFMT_TSPEC, &info, &timeout) >= 0) {
                    
                    uint64_t t_base_ns = (static_cast<uint64_t>(info.assert_timestamp.tv_sec) * 1000000000ULL) + 
                                         info.assert_timestamp.tv_nsec;

                    // --- FRAME A (t = 0.0s) ---
                    cam0->TriggerSoftware.Execute();
                    cam1->TriggerSoftware.Execute();

                    try {
                        ImagePtr img0 = cam0->GetNextImage(1000);
                        ImagePtr img1 = cam1->GetNextImage(1000);

                        if (!img0->IsIncomplete() && !img1->IsIncomplete()) {
                            buffer->push(static_cast<const uint8_t*>(img0->GetData()),
                                         static_cast<const uint8_t*>(img1->GetData()), 
                                         t_base_ns);
                        }
                        img0 = nullptr;
                        img1 = nullptr;
                    }
                    catch (Spinnaker::Exception& e) {
                        if (keep_running) std::cerr << "[EXCEPTION RUNTIME A] " << e.what() << std::endl;
                    }

                    // --- INTERPOLATION (t = +500ms) ---
                    auto target_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    std::this_thread::sleep_until(target_time);

                    if (!keep_running) break;

                    uint64_t t_half_ns = t_base_ns + 500000000ULL;

                    // --- FRAME B (t = 0.5s) ---
                    cam0->TriggerSoftware.Execute();
                    cam1->TriggerSoftware.Execute();

                    try {
                        ImagePtr img0 = cam0->GetNextImage(1000);
                        ImagePtr img1 = cam1->GetNextImage(1000);

                        if (!img0->IsIncomplete() && !img1->IsIncomplete()) {
                            buffer->push(static_cast<const uint8_t*>(img0->GetData()),
                                         static_cast<const uint8_t*>(img1->GetData()), 
                                         t_half_ns);
                        }
                        img0 = nullptr;
                        img1 = nullptr;
                    }
                    catch (Spinnaker::Exception& e) {
                        if (keep_running) std::cerr << "[EXCEPTION RUNTIME B] " << e.what() << std::endl;
                    }

                } else {
                    if (keep_running) std::cerr << "[WARN] Timeout PPS." << std::endl;
                }
            }

            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam0->EndAcquisition();
            cam1->EndAcquisition();
            cam0->DeInit();
            cam1->DeInit();
        }
        catch (Spinnaker::Exception& e) {
            std::cerr << "[EXCEPTION PIPELINE] " << e.what() << std::endl;
        }

        cam0 = nullptr;
        cam1 = nullptr;
        camList.Clear();
    } 

    time_pps_destroy(pps_handle);
    close(pps_fd);
    buffer->shutdown();
}

int main() {
    std::signal(SIGINT, sigint_handler);
    SystemPtr system = System::GetInstance();
    size_t payload_bytes = 0;

    {
        CameraList camList = system->GetCameras();
        if (camList.GetSize() == 0) {
            std::cerr << "[ERREUR FATALE] Aucune caméra détectée." << std::endl;
            camList.Clear();
            system->ReleaseInstance();
            return -1;
        }

        CameraPtr pCam = camList.GetByIndex(0);
        pCam->Init();
        payload_bytes = pCam->PayloadSize.GetValue();
        pCam->DeInit();
        
        pCam = nullptr;
        camList.Clear();
    }

    size_t RING_BUFFER_SIZE = 60;
    auto ring_buffer = std::make_shared<RingBuffer>(RING_BUFFER_SIZE, payload_bytes);

    std::thread t2(consumer_thread, ring_buffer);
    std::thread t1(producer_thread, system, ring_buffer);

    t1.join();
    t2.join();

    system->ReleaseInstance();
    std::cout << "[SYSTEM] Processus terminé avec succès." << std::endl;
    return 0;
}