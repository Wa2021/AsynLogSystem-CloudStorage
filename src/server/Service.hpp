#pragma once

#include <string>
#include <vector>

#include <event2/http.h>

#include "DataManager.hpp"

struct event_base;

namespace storage
{
    class Service
    {
    public:
        explicit Service(DataManager &data_manager);
        bool RunModule();

    private:
        static void GenericHandler(evhttp_request *request, void *context);
        static void SignalHandler(evutil_socket_t signal, short events,
                                  void *context);

        void HandleRequest(evhttp_request *request);
        void ShowLogin(evhttp_request *request);
        void Login(evhttp_request *request);
        void Logout(evhttp_request *request);
        void Upload(evhttp_request *request);
        void Delete(evhttp_request *request);
        void ListFiles(evhttp_request *request);
        void Download(evhttp_request *request, const std::string &path);

        std::string GenerateFileList(const std::vector<StorageInfo> &files) const;

    private:
        DataManager &data_manager_;
    };
} // namespace storage
