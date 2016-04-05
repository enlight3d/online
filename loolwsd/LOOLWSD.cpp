/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

// This is the main source for the loolwsd program. LOOL uses several loolwsd processes: one main
// parent process that listens on the TCP port and accepts connections from LOOL clients, and a
// number of child processes, each which handles a viewing (editing) session for one document.

#include <errno.h>
#include <locale.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <time.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

#include <Poco/DOM/AutoPtr.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/Document.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/ConsoleCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/KeyConsoleHandler.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/Net.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Path.h>
#include <Poco/Process.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/StreamCopier.h>
#include <Poco/StringTokenizer.h>
#include <Poco/TemporaryFile.h>
#include <Poco/ThreadPool.h>
#include <Poco/URI.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

#include "Admin.hpp"
#include "Auth.hpp"
#include "ChildProcessSession.hpp"
#include "Common.hpp"
#include "FileServer.hpp"
#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "MasterProcessSession.hpp"
#include "QueueHandler.hpp"
#include "Storage.hpp"
#include "IoUtil.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::Exception;
using Poco::File;
using Poco::FileOutputStream;
using Poco::IOException;
using Poco::Net::HTMLForm;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::MessageHeader;
using Poco::Net::NameValueCollection;
using Poco::Net::PartHandler;
using Poco::Net::SecureServerSocket;
using Poco::Net::ServerSocket;
using Poco::Net::Socket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Path;
using Poco::Process;
using Poco::ProcessHandle;
using Poco::Runnable;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::TemporaryFile;
using Poco::Thread;
using Poco::ThreadPool;
using Poco::URI;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;
using Poco::XML::AutoPtr;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::Element;
using Poco::XML::InputSource;
using Poco::XML::Node;
using Poco::XML::NodeList;

/// New LOK child processes ready to host documents.
//TODO: Move to a more sensible namespace.
static std::vector<std::shared_ptr<ChildProcess>> newChildren;
static std::mutex newChildrenMutex;
static std::condition_variable newChildrenCV;
static std::map<std::string, std::shared_ptr<DocumentBroker>> docBrokers;
static std::mutex docBrokersMutex;

void forkChildren(int number)
{
    assert(!newChildrenMutex.try_lock()); // check it is held.

    const std::string aMessage = "spawn " + std::to_string(number) + "\n";
    Log::debug("MasterToBroker: " + aMessage.substr(0, aMessage.length() - 1));
    IoUtil::writeFIFO(LOOLWSD::BrokerWritePipe, aMessage);
}

void preForkChildren()
{
    std::unique_lock<std::mutex> lock(newChildrenMutex);
    forkChildren(LOOLWSD::NumPreSpawnedChildren);
}

std::shared_ptr<ChildProcess> getNewChild()
{
    std::unique_lock<std::mutex> lock(newChildrenMutex);

    const int available = newChildren.size();
    int balance = LOOLWSD::NumPreSpawnedChildren;
    if (available == 0)
    {
        Log::error("No available child. Sending spawn request to Broker and failing.");
    }
    else
    {
        balance -= available - 1;
    }

    if (balance > 0)
        forkChildren(balance);

    const auto timeout = std::chrono::milliseconds(CHILD_TIMEOUT_SECS * 1000);
    if (newChildrenCV.wait_for(lock, timeout, [](){ return !newChildren.empty(); }))
    {
        auto child = newChildren.back();
        newChildren.pop_back();
        return child;
    }

    return nullptr;
}

/// Handles the filename part of the convert-to POST request payload.
class ConvertToPartHandler : public PartHandler
{
    std::string& _filename;
public:
    ConvertToPartHandler(std::string& filename)
        : _filename(filename)
    {
    }

    virtual void handlePart(const MessageHeader& header, std::istream& stream) override
    {
        // Extract filename and put it to a temporary directory.
        std::string disp;
        NameValueCollection params;
        if (header.has("Content-Disposition"))
        {
            std::string cd = header.get("Content-Disposition");
            MessageHeader::splitParameters(cd, disp, params);
        }

        if (!params.has("filename"))
            return;

        Path tempPath = Path::forDirectory(TemporaryFile().tempName() + Path::separator());
        File(tempPath).createDirectories();
        tempPath.setFileName(params.get("filename"));
        _filename = tempPath.toString();

        // Copy the stream to _filename.
        std::ofstream fileStream;
        fileStream.open(_filename);
        StreamCopier::copyStream(stream, fileStream);
        fileStream.close();
    }
};

/// Handle a public connection from a client.
class ClientRequestHandler: public HTTPRequestHandler
{
private:

    void handlePostRequest(HTTPServerRequest& request, HTTPServerResponse& response, const std::string& id)
    {
        Log::info("Post request: [" + request.getURI() + "]");
        StringTokenizer tokens(request.getURI(), "/?");
        if (tokens.count() >= 2 && tokens[1] == "convert-to")
        {
            std::string fromPath;
            ConvertToPartHandler handler(fromPath);
            HTMLForm form(request, request.stream(), handler);
            const std::string format = (form.has("format") ? form.get("format") : "");

            bool sent = false;
            if (!fromPath.empty())
            {
                if (!format.empty())
                {
                    Log::info("Conversion request for URI [" + fromPath + "].");

                    // Request a kit process for this doc.
                    auto child = getNewChild();
                    if (!child)
                    {
                        // Let the client know we can't serve now.
                        response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
                        response.setContentLength(0);
                        response.send();
                        return;
                    }

                    auto uriPublic = DocumentBroker::sanitizeURI(fromPath);
                    const auto docKey = DocumentBroker::getDocKey(uriPublic);
                    auto docBroker = std::make_shared<DocumentBroker>(uriPublic, docKey, LOOLWSD::ChildRoot, child);

                    // This lock could become a bottleneck.
                    // In that case, we can use a pool and index by publicPath.
                    std::unique_lock<std::mutex> lock(docBrokersMutex);

                    //FIXME: What if the same document is already open? Need a fake dockey here.
                    Log::debug("New DocumentBroker for docKey [" + docKey + "].");
                    docBrokers.emplace(docKey, docBroker);

                    // Load the document.
                    std::shared_ptr<WebSocket> ws;
                    auto session = std::make_shared<MasterProcessSession>(id, LOOLSession::Kind::ToClient, ws, docBroker, nullptr);
                    docBroker->addWSSession(id, session);
                    unsigned wsSessionsCount = docBroker->getWSSessionsCount();
                    Log::warn(docKey + ", ws_sessions++: " + std::to_string(wsSessionsCount));
                    session->setEditLock(true);
                    docBroker->incSessions();
                    lock.unlock();

                    std::string encodedFrom;
                    URI::encode(docBroker->getPublicUri().getPath(), "", encodedFrom);
                    const std::string load = "load url=" + encodedFrom;
                    session->handleInput(load.data(), load.size());

                    // Convert it to the requested format.
                    Path toPath(docBroker->getPublicUri().getPath());
                    toPath.setExtension(format);
                    const std::string toJailURL = "file://" + std::string(JAILED_DOCUMENT_ROOT) + toPath.getFileName();
                    std::string encodedTo;
                    URI::encode(toJailURL, "", encodedTo);
                    std::string saveas = "saveas url=" + encodedTo + " format=" + format + " options=";
                    session->handleInput(saveas.data(), saveas.size());

                    // Send it back to the client.
                    //TODO: Should have timeout to avoid waiting forever.
                    Poco::URI resultURL(session->getSaveAs());
                    if (!resultURL.getPath().empty())
                    {
                        const std::string mimeType = "application/octet-stream";
                        response.sendFile(resultURL.getPath(), mimeType);
                        sent = true;
                    }

                    lock.lock();
                    if (docBroker->decSessions() == 0)
                    {
                        Log::debug("Removing DocumentBroker for docKey [" + docKey + "].");
                        docBrokers.erase(docKey);
                    }
                }

                // Clean up the temporary directory the HTMLForm ctor created.
                Path tempDirectory(fromPath);
                tempDirectory.setFileName("");
                Util::removeFile(tempDirectory, /*recursive=*/true);
            }

            if (!sent)
            {
                response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                response.setContentLength(0);
                response.send();
            }
        }
        else if (tokens.count() >= 2 && tokens[1] == "insertfile")
        {
            Log::info("Insert file request.");
            response.set("Access-Control-Allow-Origin", "*");
            response.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");

            std::string tmpPath;
            ConvertToPartHandler handler(tmpPath);
            HTMLForm form(request, request.stream(), handler);

            bool goodRequest = form.has("childid") && form.has("name");
            std::string formChildid(form.get("childid"));
            std::string formName(form.get("name"));

            // protect against attempts to inject something funny here
            if (goodRequest && formChildid.find('/') != std::string::npos && formName.find('/') != std::string::npos)
                goodRequest = false;

            if (goodRequest)
            {
                try
                {
                    Log::info() << "Perform insertfile: " << formChildid << ", " << formName << Log::end;
                    const std::string dirPath = LOOLWSD::ChildRoot + formChildid
                                              + JAILED_DOCUMENT_ROOT + "insertfile";
                    File(dirPath).createDirectories();
                    std::string fileName = dirPath + Path::separator() + form.get("name");
                    File(tmpPath).moveTo(fileName);

                    response.setStatus(HTTPResponse::HTTP_OK);
                    response.send();
                }
                catch (const IOException& exc)
                {
                    Log::info() << "ClientRequestHandler::handlePostRequest: IOException: " << exc.message() << Log::end;
                    response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                    response.send();
                }
            }
            else
            {
                response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
                response.send();
            }
        }
        else if (tokens.count() >= 4)
        {
            Log::info("File download request.");
            // The user might request a file to download
            const std::string dirPath = LOOLWSD::ChildRoot + tokens[1]
                                      + JAILED_DOCUMENT_ROOT + tokens[2];
            std::string fileName;
            URI::decode(tokens[3], fileName);
            const std::string filePath = dirPath + Path::separator() + fileName;
            Log::info("HTTP request for: " + filePath);
            File file(filePath);
            if (file.exists())
            {
                response.set("Access-Control-Allow-Origin", "*");
                HTMLForm form(request);
                std::string mimeType = "application/octet-stream";
                if (form.has("mime_type"))
                    mimeType = form.get("mime_type");
                response.sendFile(filePath, mimeType);
                Util::removeFile(dirPath, true);
            }
            else
            {
                response.setStatus(HTTPResponse::HTTP_NOT_FOUND);
                response.setContentLength(0);
                response.send();
            }
        }
        else
        {
            Log::info("Bad request.");
            response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
            response.setContentLength(0);
            response.send();
        }
    }

    void handleGetRequest(HTTPServerRequest& request, HTTPServerResponse& response, const std::string& id)
    {
        Log::info("Starting GET request handler for session [" + id + "].");

        // Remove the leading '/' in the GET URL.
        std::string uri = request.getURI();
        if (uri.size() > 0 && uri[0] == '/')
        {
            uri.erase(0, 1);
        }

        const auto uriPublic = DocumentBroker::sanitizeURI(uri);
        const auto docKey = DocumentBroker::getDocKey(uriPublic);
        std::shared_ptr<DocumentBroker> docBroker;
        // This lock could become a bottleneck.
        // In that case, we can use a pool and index by publicPath.
        std::unique_lock<std::mutex> docBrokersLock(docBrokersMutex);

        // Lookup this document.
        auto it = docBrokers.find(docKey);
        if (it != docBrokers.end())
        {
            // Get the DocumentBroker from the Cache.
            Log::debug("Found DocumentBroker for docKey [" + docKey + "].");
            docBroker = it->second;
            assert(docBroker);
        }
        else
        {
            // Request a kit process for this doc.
            auto child = getNewChild();
            if (!child)
            {
                // Let the client know we can't serve now.
                response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
                response.setContentLength(0);
                response.send();
                return;
            }

            // Set one we just created.
            Log::debug("New DocumentBroker for docKey [" + docKey + "].");
            docBroker = std::make_shared<DocumentBroker>(uriPublic, docKey, LOOLWSD::ChildRoot, child);
            docBrokers.emplace(docKey, docBroker);
        }

        // Validate the URI and Storage before moving on.
        docBroker->validate(uriPublic);
        Log::debug("Validated [" + uriPublic.toString() + "].");

        // For ToClient sessions, we store incoming messages in a queue and have a separate
        // thread that handles them. This is so that we can empty the queue when we get a
        // "canceltiles" message.
        auto queue = std::make_shared<BasicTileQueue>();

        auto ws = std::make_shared<WebSocket>(request, response);
        auto session = std::make_shared<MasterProcessSession>(id, LOOLSession::Kind::ToClient, ws, docBroker, queue);
        docBroker->incSessions();
        docBrokersLock.unlock();

        docBroker->addWSSession(id, session);
        unsigned wsSessionsCount = docBroker->getWSSessionsCount();
        Log::warn(docKey + ", ws_sessions++: " + std::to_string(wsSessionsCount));
        if (wsSessionsCount == 1)
            session->setEditLock(true);

        QueueHandler handler(queue, session, "wsd_queue_" + session->getId());

        Thread queueHandlerThread;
        queueHandlerThread.start(handler);
        bool normalShutdown = false;

        IoUtil::SocketProcessor(ws, response,
                [&session, &queue, &normalShutdown](const std::vector<char>& payload)
            {
                time(&session->_lastMessageTime);
                const auto token = LOOLProtocol::getFirstToken(payload);
                if (token == "disconnect")
                {
                    normalShutdown = true;
                }
                else
                {
                    queue->put(payload);
                }

                return true;
            },
            []() { return TerminationFlag; },
            "Client_ws_" + id
            );

        if (docBroker->getSessionsCount() == 1 && !normalShutdown && !session->_bLoadError)
        {
            //TODO: This isn't this simple. We need to wait for the notification
            // of save so Storage can persist the save (if necessary).
            Log::info("Non-deliberate shutdown of the last session, saving the document before tearing down.");
            queue->put("uno .uno:Save");
        }
        else
        {
            Log::info("Clearing the queue.");
            queue->clear();
        }

        docBroker->removeWSSession(id);
        wsSessionsCount = docBroker->getWSSessionsCount();
        Log::warn(docKey + ", ws_sessions--: " + std::to_string(wsSessionsCount));

        Log::info("Finishing GET request handler for session [" + id + "]. Joining the queue.");
        queue->put("eof");
        queueHandlerThread.join();

        docBrokersLock.lock();
        if (docBroker->decSessions() == 0)
        {
            Log::debug("Removing DocumentBroker for docKey [" + docKey + "].");
            docBrokers.erase(docKey);
        }
    }

    void handleGetDiscovery(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        DOMParser parser;
        DOMWriter writer;
        URI uri("http", request.getHost(), request.getURI());

        const std::string discoveryPath = Path(Application::instance().commandPath()).parent().toString() + "discovery.xml";
        const std::string mediaType = "text/xml";
        const std::string action = "action";
        const std::string urlsrc = "urlsrc";
        const std::string uriValue = "https://" + uri.getHost() + ":" + std::to_string(uri.getPort()) + "/loleaflet/dist/loleaflet.html?";

        InputSource inputSrc(discoveryPath);
        AutoPtr<Poco::XML::Document> docXML = parser.parse(&inputSrc);
        AutoPtr<NodeList> listNodes = docXML->getElementsByTagName(action);

        for (unsigned long it = 0; it < listNodes->length(); it++)
        {
            static_cast<Element*>(listNodes->item(it))->setAttribute(urlsrc, uriValue);
        }

        std::ostringstream ostrXML;
        writer.writeNode(ostrXML, docXML);

        response.set("User-Agent", "LOOLWSD WOPI Agent");
        response.setContentLength(ostrXML.str().length());
        response.setContentType(mediaType);
        response.setChunkedTransferEncoding(false);

        std::ostream& ostr = response.send();
        ostr << ostrXML.str();
    }

public:

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        const auto id = LOOLWSD::GenSessionId();
        const std::string thread_name = "client_ws_" + id;

        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(thread_name.c_str()), 0, 0, 0) != 0)
            Log::error("Cannot set thread name to " + thread_name + ".");

        Log::debug("Thread [" + thread_name + "] started.");

        try
        {
            if (request.getMethod() == HTTPRequest::HTTP_GET && request.getURI() == "/hosting/discovery")
            {
                // http://server/hosting/discovery
                handleGetDiscovery(request, response);
            }
            else if (!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0))
            {
                handlePostRequest(request, response, id);
            }
            else
            {
                //authenticate(request, response, id);
                handleGetRequest(request, response, id);
            }
        }
        catch (const Exception& exc)
        {
            Log::error() << "ClientRequestHandler::handleRequest: Exception: " << exc.displayText()
                         << (exc.nested() ? " (" + exc.nested()->displayText() + ")" : "")
                         << Log::end;
        }
        catch (const std::exception& exc)
        {
            Log::error(std::string("ClientRequestHandler::handleRequest: Exception: ") + exc.what());
        }
        catch (...)
        {
            Log::error("ClientRequestHandler::handleRequest: Unexpected exception");
        }

        Log::debug("Thread [" + thread_name + "] finished.");
    }
};

/// Handle requests from prisoners (internal).
class PrisonerRequestHandler: public HTTPRequestHandler
{
public:

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        std::string thread_name = "prison_ws_";
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(thread_name.c_str()), 0, 0, 0) != 0)
            Log::error("Cannot set thread name to " + thread_name + ".");

        Log::debug("Child connection with URI [" + request.getURI() + "].");

        assert(request.serverAddress().port() == MASTER_PORT_NUMBER);
        if (request.getURI().find(NEW_CHILD_URI) == 0)
        {
            // New Child is spawned.
            const auto params = Poco::URI(request.getURI()).getQueryParameters();
            Poco::Process::PID pid = -1;
            for (const auto& param : params)
            {
                if (param.first == "pid")
                {
                    pid = std::stoi(param.second);
                }
            }

            if (pid <= 0)
            {
                Log::error("Invalid PID in child URI [" + request.getURI() + "].");
                return;
            }

            Log::info("New child [" + std::to_string(pid) + "].");
            auto ws = std::make_shared<WebSocket>(request, response);
            std::unique_lock<std::mutex> lock(newChildrenMutex);
            newChildren.emplace_back(std::make_shared<ChildProcess>(pid, ws));
            Log::info("Have " + std::to_string(newChildren.size()) + " children.");
            newChildrenCV.notify_one();
            return;
        }

        if (request.getURI().find(CHILD_URI) != 0)
        {
            Log::error("Invalid request URI: [" + request.getURI() + "].");
            return;
        }

        try
        {
            const auto params = Poco::URI(request.getURI()).getQueryParameters();
            std::string sessionId;
            std::string jailId;
            std::string docKey;
            for (const auto& param : params)
            {
                if (param.first == "sessionId")
                {
                    sessionId = param.second;
                }
                else if (param.first == "jailId")
                {
                    jailId = param.second;
                }
                else if (param.first == "docKey")
                {
                    docKey = param.second;
                }
            }

            thread_name += sessionId;
            if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(thread_name.c_str()), 0, 0, 0) != 0)
                Log::error("Cannot set thread name to " + thread_name + ".");

            Log::debug("Thread [" + thread_name + "] started.");

            Log::debug("Child socket for SessionId: " + sessionId + ", jailId: " + jailId +
                       ", docKey: " + docKey + " connected.");

            std::shared_ptr<DocumentBroker> docBroker;
            {
                // This lock could become a bottleneck.
                // In that case, we can use a pool and index by publicPath.
                std::unique_lock<std::mutex> lock(docBrokersMutex);

                // Lookup this document.
                auto it = docBrokers.find(docKey);
                if (it != docBrokers.end())
                {
                    // Get the DocumentBroker from the Cache.
                    docBroker = it->second;
                    assert(docBroker);
                }
                else
                {
                    // The client closed before we started,
                    // or some early failure happened.
                    Log::error("Failed to find DocumentBroker for docKey [" + docKey +
                               "] while handling child connection for session [" + sessionId + "].");
                    throw std::runtime_error("Invalid docKey.");
                }
            }

            docBroker->load(jailId);

            auto ws = std::make_shared<WebSocket>(request, response);
            auto session = std::make_shared<MasterProcessSession>(sessionId, LOOLSession::Kind::ToPrisoner, ws, docBroker, nullptr);

            std::unique_lock<std::mutex> lock(MasterProcessSession::AvailableChildSessionMutex);
            MasterProcessSession::AvailableChildSessions.emplace(sessionId, session);

            Log::info() << " mapped " << session << " jailId=" << jailId << ", id=" << sessionId
                        << " into _availableChildSessions, size=" << MasterProcessSession::AvailableChildSessions.size() << Log::end;

            lock.unlock();
            MasterProcessSession::AvailableChildSessionCV.notify_one();

            IoUtil::SocketProcessor(ws, response,
                    [&session](const std::vector<char>& payload)
                {
                    return session->handleInput(payload.data(), payload.size());
                },
                []() { return TerminationFlag; },
                "Child_ws_" + sessionId
                );
        }
        catch (const Exception& exc)
        {
            Log::error() << "PrisonerRequestHandler::handleRequest: Exception: " << exc.displayText()
                         << (exc.nested() ? " (" + exc.nested()->displayText() + ")" : "")
                         << Log::end;
        }
        catch (const std::exception& exc)
        {
            Log::error(std::string("PrisonerRequestHandler::handleRequest: Exception: ") + exc.what());
        }
        catch (...)
        {
            Log::error("PrisonerRequestHandler::handleRequest: Unexpected exception");
        }

        Log::debug("Thread [" + thread_name + "] finished.");
    }
};

class ClientRequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
    ClientRequestHandlerFactory(Admin& admin, FileServer& fileServer)
        : _admin(admin),
          _fileServer(fileServer)
        { }

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>("request_handler"), 0, 0, 0) != 0)
            Log::error("Cannot set thread name to request_handler.");

        auto logger = Log::info();
        logger << "Request from " << request.clientAddress().toString() << ": "
               << request.getMethod() << " " << request.getURI() << " "
               << request.getVersion();

        for (HTTPServerRequest::ConstIterator it = request.begin(); it != request.end(); ++it)
        {
            logger << " / " << it->first << ": " << it->second;
        }

        logger << Log::end;

        // Routing
        // FIXME: Some browsers (all?) hit for /favicon.ico. Create a nice favicon and add to routes
        Poco::URI requestUri(request.getURI());
        std::vector<std::string> reqPathSegs;
        requestUri.getPathSegments(reqPathSegs);
        HTTPRequestHandler* requestHandler;

        // File server
        if (reqPathSegs.size() >= 1 && reqPathSegs[0] == "loleaflet")
        {
            requestHandler = _fileServer.createRequestHandler();
        }
        // Admin WebSocket Connections
        else if (reqPathSegs.size() >= 1 && reqPathSegs[0] == "adminws")
        {
            requestHandler = _admin.createRequestHandler();
        }
        // Client post and websocket connections
        else
        {
            requestHandler = new ClientRequestHandler();
        }

        return requestHandler;
    }

private:
    Admin& _admin;
    FileServer& _fileServer;
};

class PrisonerRequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>("request_handler"), 0, 0, 0) != 0)
            Log::error("Cannot set thread name to request_handler.");

        auto logger = Log::info();
        logger << "Request from " << request.clientAddress().toString() << ": "
               << request.getMethod() << " " << request.getURI() << " "
               << request.getVersion();

        for (HTTPServerRequest::ConstIterator it = request.begin(); it != request.end(); ++it)
        {
            logger << " / " << it->first << ": " << it->second;
        }

        logger << Log::end;
        return new PrisonerRequestHandler();
    }
};

class TestOutput : public Runnable
{
public:
    TestOutput(WebSocket& ws) :
        _ws(ws)
    {
    }

    void run() override
    {
        int flags;
        int n;
        _ws.setReceiveTimeout(0);
        try
        {
            do
            {
                char buffer[200000];
                n = _ws.receiveFrame(buffer, sizeof(buffer), flags);
                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                {
                    Log::trace() << "Client got " << n << " bytes: "
                                 << getAbbreviatedMessage(buffer, n) << Log::end;
                }
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        }
        catch (const WebSocketException& exc)
        {
            Log::error("TestOutput::run(), WebSocketException: " + exc.message());
            _ws.close();
        }
    }

private:
    WebSocket& _ws;
};

class TestInput : public Runnable
{
public:
    TestInput(ServerApplication& main, ServerSocket& svs, HTTPServer& srv) :
        _main(main),
        _svs(svs),
        _srv(srv)
    {
    }

    void run() override
    {
        HTTPClientSession cs("127.0.0.1", _svs.address().port());
        HTTPRequest request(HTTPRequest::HTTP_GET, "/ws");
        HTTPResponse response;
        WebSocket ws(cs, request, response);

        Thread thread;
        TestOutput output(ws);
        thread.start(output);

        if (isatty(0))
        {
            std::cout << std::endl;
            std::cout << "Enter LOOL WS requests, one per line. Enter EOF to finish." << std::endl;
        }

        while (!std::cin.eof())
        {
            std::string line;
            std::getline(std::cin, line);
            ws.sendFrame(line.c_str(), line.size());
        }
        thread.join();
        _srv.stopAll();
        _main.terminate();
    }

private:
    ServerApplication& _main;
    ServerSocket& _svs;
    HTTPServer& _srv;
};

std::atomic<unsigned> LOOLWSD::NextSessionId;
int LOOLWSD::BrokerWritePipe = -1;
std::string LOOLWSD::Cache = LOOLWSD_CACHEDIR;
std::string LOOLWSD::SysTemplate;
std::string LOOLWSD::LoTemplate;
std::string LOOLWSD::ChildRoot;
std::string LOOLWSD::LoSubPath = "lo";
std::string LOOLWSD::FileServerRoot;

int LOOLWSD::NumPreSpawnedChildren = 10;
bool LOOLWSD::DoTest = false;
static const std::string pidLog = "/tmp/loolwsd.pid";

LOOLWSD::LOOLWSD()
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::initialize(Application& self)
{
    // load default configuration files, if present
    if (loadConfiguration() == 0)
    {
        std::string configPath = LOOLWSD_CONFIGDIR "/loolwsd.xml";
        loadConfiguration(configPath);
    }

    ServerApplication::initialize(self);
}

void LOOLWSD::initializeSSL()
{
    auto& conf = config();

    auto ssl_cert_file_path = conf.getString("ssl.cert_file_path");
    if (conf.getBool("ssl.cert_file_path[@relative]"))
    {
        ssl_cert_file_path = Poco::Path(Application::instance().commandPath()).parent().append(ssl_cert_file_path).toString();
    }

    Log::info("SSL Cert file: " + ssl_cert_file_path);

    auto ssl_key_file_path = conf.getString("ssl.key_file_path");
    if (conf.getBool("ssl.key_file_path[@relative]"))
    {
        ssl_key_file_path = Poco::Path(Application::instance().commandPath()).parent().append(ssl_key_file_path).toString();
    }

    Log::info("SSL Key file: " + ssl_key_file_path);

    auto ssl_ca_file_path = conf.getString("ssl.ca_file_path");
    if (conf.getBool("ssl.ca_file_path[@relative]"))
    {
        ssl_ca_file_path = Poco::Path(Application::instance().commandPath()).parent().append(ssl_ca_file_path).toString();
    }

    Log::info("SSL CA file: " + ssl_ca_file_path);

    Poco::Crypto::initializeCrypto();

    Poco::Net::initializeSSL();
    Poco::Net::Context::Params sslParams;
    sslParams.certificateFile = ssl_cert_file_path;
    sslParams.privateKeyFile = ssl_key_file_path;
    sslParams.caLocation = ssl_ca_file_path;
    // Don't ask clients for certificate
    sslParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleHandler = new Poco::Net::KeyConsoleHandler(true);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidCertHandler = new Poco::Net::ConsoleCertificateHandler(true);

    Poco::Net::Context::Ptr sslContext = new Poco::Net::Context(Poco::Net::Context::SERVER_USE, sslParams);
    Poco::Net::SSLManager::instance().initializeServer(consoleHandler, invalidCertHandler, sslContext);

    // Init client
    Poco::Net::Context::Params sslClientParams;
    // TODO: Be more strict and setup SSL key/certs for owncloud server and us
    sslClientParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleClientHandler = new Poco::Net::KeyConsoleHandler(false);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidClientCertHandler = new Poco::Net::AcceptCertificateHandler(false);

    Poco::Net::Context::Ptr sslClientContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslClientParams);
    Poco::Net::SSLManager::instance().initializeClient(consoleClientHandler, invalidClientCertHandler, sslClientContext);
}

void LOOLWSD::uninitialize()
{
    ServerApplication::uninitialize();
}

void LOOLWSD::defineOptions(OptionSet& optionSet)
{
    ServerApplication::defineOptions(optionSet);

    optionSet.addOption(Option("help", "", "Display help information on command line arguments.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("version", "", "Display version information.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("port", "", "Port number to listen to (default: " + std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "),"
                             " must not be " + std::to_string(MASTER_PORT_NUMBER) + ".")
                        .required(false)
                        .repeatable(false)
                        .argument("port number"));

    optionSet.addOption(Option("cache", "", "Path to a directory where to keep the persistent tile cache (default: " + std::string(LOOLWSD_CACHEDIR) + ").")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("systemplate", "", "Path to a template tree with shared libraries etc to be used as source for chroot jails for child processes.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("lotemplate", "", "Path to a LibreOffice installation tree to be copied (linked) into the jails for child processes. Should be on the same file system as systemplate.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("childroot", "", "Path to the directory under which the chroot jails for the child processes will be created. Should be on the same file system as systemplate and lotemplate.")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("losubpath", "", "Relative path where the LibreOffice installation will be copied inside a jail (default: '" + LoSubPath + "').")
                        .required(false)
                        .repeatable(false)
                        .argument("relative path"));

    optionSet.addOption(Option("fileserverroot", "", "Path to the directory that should be considered root for the file server (default: '../loleaflet/').")
                        .required(false)
                        .repeatable(false)
                        .argument("directory"));

    optionSet.addOption(Option("numprespawns", "", "Number of child processes to keep started in advance and waiting for new clients.")
                        .required(false)
                        .repeatable(false)
                        .argument("number"));

    optionSet.addOption(Option("test", "", "Interactive testing.")
                        .required(false)
                        .repeatable(false));
}

void LOOLWSD::handleOption(const std::string& optionName, const std::string& value)
{
    ServerApplication::handleOption(optionName, value);

    if (optionName == "help")
    {
        displayHelp();
        std::exit(Application::EXIT_OK);
    }
    else if (optionName == "version")
    {
        displayVersion();
        std::exit(Application::EXIT_OK);
    }
    else if (optionName == "port")
        ClientPortNumber = std::stoi(value);
    else if (optionName == "cache")
        Cache = value;
    else if (optionName == "systemplate")
        SysTemplate = value;
    else if (optionName == "lotemplate")
        LoTemplate = value;
    else if (optionName == "childroot")
        ChildRoot = value;
    else if (optionName == "losubpath")
        LoSubPath = value;
    else if (optionName == "fileserverroot")
        FileServerRoot = value;
    else if (optionName == "numprespawns")
        NumPreSpawnedChildren = std::stoi(value);
    else if (optionName == "test")
        LOOLWSD::DoTest = true;
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice On-Line WebSocket server.");
    helpFormatter.format(std::cout);
}

void LOOLWSD::displayVersion()
{
    std::cout << LOOLWSD_VERSION << std::endl;
}

Process::PID LOOLWSD::createBroker()
{
    Process::Args args;

    args.push_back("--losubpath=" + LOOLWSD::LoSubPath);
    args.push_back("--systemplate=" + SysTemplate);
    args.push_back("--lotemplate=" + LoTemplate);
    args.push_back("--childroot=" + ChildRoot);
    args.push_back("--clientport=" + std::to_string(ClientPortNumber));

    const std::string brokerPath = Path(Application::instance().commandPath()).parent().toString() + "loolforkit";

    Log::info("Launching kit forker #1: " + brokerPath + " " +
              Poco::cat(std::string(" "), args.begin(), args.end()));

    ProcessHandle child = Process::launch(brokerPath, args);

    return child.id();
}

int LOOLWSD::main(const std::vector<std::string>& /*args*/)
{
    Log::initialize("wsd");

    if (geteuid() == 0)
    {
        Log::error("Don't run this as root");
        return Application::EXIT_USAGE;
    }

    initializeSSL();

    char *locale = setlocale(LC_ALL, nullptr);
    if (locale == nullptr || std::strcmp(locale, "C") == 0)
        setlocale(LC_ALL, "en_US.utf8");

    Util::setTerminationSignals();
    Util::setFatalSignals();

    if (access(Cache.c_str(), R_OK | W_OK | X_OK) != 0)
    {
        Log::error("Unable to access cache [" + Cache +
                   "] please make sure it exists, and has write permission for this user.");
        return Application::EXIT_SOFTWARE;
    }

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (SysTemplate.empty())
        throw MissingOptionException("systemplate");
    if (LoTemplate.empty())
        throw MissingOptionException("lotemplate");

    if (ChildRoot.empty())
        throw MissingOptionException("childroot");
    else if (ChildRoot[ChildRoot.size() - 1] != Path::separator())
        ChildRoot += Path::separator();

    if (FileServerRoot.empty())
        FileServerRoot = Path(Application::instance().commandPath()).parent().parent().toString();

    if (ClientPortNumber == MASTER_PORT_NUMBER)
        throw IncompatibleOptionsException("port");

    if (LOOLWSD::DoTest)
        NumPreSpawnedChildren = 1;

    // log pid information
    {
        FileOutputStream filePID(pidLog);
        if (filePID.good())
            filePID << Process::id();
    }

    const Path pipePath = Path::forDirectory(ChildRoot + Path::separator() + FIFO_PATH);
    if (!File(pipePath).exists() && !File(pipePath).createDirectory())
    {
        Log::error("Error: Failed to create pipe directory [" + pipePath.toString() + "].");
        return Application::EXIT_SOFTWARE;
    }

    const std::string pipeLoolwsd = Path(pipePath, FIFO_LOOLWSD).toString();
    if (mkfifo(pipeLoolwsd.c_str(), 0666) < 0 && errno != EEXIST)
    {
        Log::error("Error: Failed to create pipe FIFO [" + pipeLoolwsd + "].");
        return Application::EXIT_SOFTWARE;
    }

    // Open notify pipe
    int pipeFlags = O_RDONLY | O_NONBLOCK;
    int notifyPipe = -1;
    const std::string pipeNotify = Path(pipePath, FIFO_ADMIN_NOTIFY).toString();
    if (mkfifo(pipeNotify.c_str(), 0666) < 0 && errno != EEXIST)
    {
        Log::error("Error: Failed to create pipe FIFO [" + std::string(FIFO_ADMIN_NOTIFY) + "].");
        exit(Application::EXIT_SOFTWARE);
    }

    if ((notifyPipe = open(pipeNotify.c_str(), pipeFlags) ) < 0)
    {
        Log::error("Error: pipe opened for reading.");
        exit(Application::EXIT_SOFTWARE);
    }

    if ((pipeFlags = fcntl(notifyPipe, F_GETFL, 0)) < 0)
    {
        Log::error("Error: failed to get pipe flags [" + std::string(FIFO_ADMIN_NOTIFY) + "].");
        exit(Application::EXIT_SOFTWARE);
    }

    pipeFlags &= ~O_NONBLOCK;
    if (fcntl(notifyPipe, F_SETFL, pipeFlags) < 0)
    {
        Log::error("Error: failed to set pipe flags [" + std::string(FIFO_ADMIN_NOTIFY) + "].");
        exit(Application::EXIT_SOFTWARE);
    }

    const Process::PID brokerPid = createBroker();
    if (brokerPid < 0)
    {
        Log::error("Failed to spawn loolBroker.");
        return Application::EXIT_SOFTWARE;
    }

    // Init the Admin manager
    Admin admin(brokerPid, notifyPipe);
    // Init the file server
    FileServer fileServer;

    // Configure the Server.
    // Note: TCPServer internally uses the default
    // ThreadPool to dispatch connections.
    // The capacity of the default ThreadPool
    // is increased to match MaxThreads.
    // We must have sufficient available threads
    // in the default ThreadPool to dispatch
    // connections, otherwise we will deadlock.
    auto params1 = new HTTPServerParams();
    params1->setMaxThreads(MAX_SESSIONS);
    auto params2 = new HTTPServerParams();
    params2->setMaxThreads(MAX_SESSIONS);

    // Start a server listening on the port for clients
    SecureServerSocket svs(ClientPortNumber);
    ThreadPool threadPool(NumPreSpawnedChildren*6, MAX_SESSIONS * 2);
    HTTPServer srv(new ClientRequestHandlerFactory(admin, fileServer), threadPool, svs, params1);

    srv.start();

    // And one on the port for child processes
    SocketAddress addr2("127.0.0.1", MASTER_PORT_NUMBER);
    ServerSocket svs2(addr2);
    HTTPServer srv2(new PrisonerRequestHandlerFactory(), threadPool, svs2, params2);

    srv2.start();

    if ( (BrokerWritePipe = open(pipeLoolwsd.c_str(), O_WRONLY) ) < 0 )
    {
        Log::error("Error: failed to open pipe [" + pipeLoolwsd + "] write only.");
        return Application::EXIT_SOFTWARE;
    }

    threadPool.start(admin);

    TestInput input(*this, svs, srv);
    Thread inputThread;
    if (LOOLWSD::DoTest)
    {
        inputThread.start(input);
        waitForTerminationRequest();
    }

    preForkChildren();

    time_t last30SecCheck = time(NULL);
    time_t lastFiveMinuteCheck = time(NULL);

    int status = 0;
    while (!TerminationFlag && !LOOLWSD::DoTest)
    {
        const pid_t pid = waitpid(brokerPid, &status, WUNTRACED | WNOHANG);
        if (pid > 0)
        {
            if (brokerPid == pid)
            {
                if (WIFEXITED(status))
                {
                    Log::info() << "Child process [" << pid << "] exited with code: "
                                << WEXITSTATUS(status) << "." << Log::end;

                    break;
                }
                else
                if (WIFSIGNALED(status))
                {
                    std::string fate = "died";
                    if (WCOREDUMP(status))
                        fate = "core-dumped";
                    Log::error() << "Child process [" << pid << "] " << fate
                                 << " with " << Util::signalName(WTERMSIG(status))
                                 << " signal: " << strsignal(WTERMSIG(status))
                                 << Log::end;

                    break;
                }
                else if (WIFSTOPPED(status))
                {
                    Log::info() << "Child process [" << pid << "] stopped with "
                                << Util::signalName(WSTOPSIG(status))
                                << " signal: " << strsignal(WTERMSIG(status))
                                << Log::end;
                }
                else if (WIFCONTINUED(status))
                {
                    Log::info() << "Child process [" << pid << "] resumed with SIGCONT."
                                << Log::end;
                }
                else
                {
                    Log::warn() << "Unknown status returned by waitpid: "
                                << std::hex << status << "." << Log::end;
                }
            }
            else
            {
                Log::error("An unknown child process died, pid: " + std::to_string(pid));
            }
        }
        else if (pid < 0)
        {
            Log::error("Error: waitpid failed.");
            // No child processes
            if (errno == ECHILD)
            {
                TerminationFlag = true;
                continue;
            }
        }
        else // pid == 0, no children have died
        {
            time_t now = time(NULL);
            if (now >= last30SecCheck + 30)
            {
                Log::trace("30-second check");
                last30SecCheck = now;

                std::unique_lock<std::mutex> docBrokersLock(docBrokersMutex);
                for (auto& brokerIt : docBrokers)
                {
                    std::unique_lock<std::mutex> sessionsLock(brokerIt.second->_wsSessionsMutex);
                    for (auto& sessionIt: brokerIt.second->_wsSessions)
                    {
                        if (sessionIt.second->_lastMessageTime > sessionIt.second->_idleSaveTime &&
                            sessionIt.second->_lastMessageTime < now - 30)
                        {
                            Log::info("Idle save triggered for session " + sessionIt.second->getId());
                            sessionIt.second->getQueue()->put("uno .uno:Save");

                            sessionIt.second->_idleSaveTime = now;
                        }
                    }
                }
            }
            if (now >= lastFiveMinuteCheck + 300)
            {
                Log::trace("Five-minute check");
                lastFiveMinuteCheck = now;

                std::unique_lock<std::mutex> docBrokersLock(docBrokersMutex);
                for (auto& brokerIt : docBrokers)
                {
                    std::unique_lock<std::mutex> sessionsLock(brokerIt.second->_wsSessionsMutex);
                    for (auto& sessionIt: brokerIt.second->_wsSessions)
                    {
                        if (sessionIt.second->_lastMessageTime >= sessionIt.second->_idleSaveTime &&
                            sessionIt.second->_lastMessageTime >= sessionIt.second->_autoSaveTime)
                        {
                            Log::info("Auto-save triggered for session " + sessionIt.second->getId());
                            sessionIt.second->getQueue()->put("uno .uno:Save");

                            sessionIt.second->_autoSaveTime = now;
                        }
                    }
                }
            }

            sleep(MAINTENANCE_INTERVAL*2);
        }
    }

    if (LOOLWSD::DoTest)
        inputThread.join();

    // stop the service, no more request
    srv.stop();
    srv2.stop();

    // close all websockets
    threadPool.joinAll();

    // Terminate child processes
    IoUtil::writeFIFO(LOOLWSD::BrokerWritePipe, "eof\n");
    Log::info("Requesting child process " + std::to_string(brokerPid) + " to terminate");
    Util::requestTermination(brokerPid);

    // wait broker process finish
    waitpid(brokerPid, &status, WUNTRACED);

    close(BrokerWritePipe);

    Log::info("Cleaning up childroot directory [" + ChildRoot + "].");
    std::vector<std::string> jails;
    File(ChildRoot).list(jails);
    for (auto& jail : jails)
    {
        const auto path = ChildRoot + jail;
        Log::info("Removing jail [" + path + "].");
        Util::removeFile(path, true);
    }

    Poco::Net::uninitializeSSL();
    Poco::Crypto::uninitializeCrypto();

    Log::info("Process [loolwsd] finished.");
    return Application::EXIT_OK;
}

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
