#include <random>
#include <string>
#include <cstring>
#include "mod_audio_stream.h"
#include <ixwebsocket/IXWebSocket.h>
#include <switch_json.h>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/
static std::string generateStreamSid();

namespace {
    extern switch_bool_t filter_json_string(switch_core_session_t *session, const char* message);
}

class AudioStreamer {
public:

    AudioStreamer(const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat, const char* initialMeta,
                    bool globalTrace, bool suppressLog): m_sessionId(uuid), m_notify(callback), m_initial_meta(initialMeta),
                                                            m_global_trace(globalTrace), m_suppress_log(suppressLog), m_playFile(0){

        webSocket.setUrl(wsUri);

        // Optional heart beat, sent every xx seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        if(heart_beat)
            webSocket.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if(deflate)
            webSocket.disablePerMessageDeflate();

        // Setup a callback to be fired when a message or an event (open, close, error) is received
        webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg){
            if (msg->type == ix::WebSocketMessageType::Message)
            {
                eventCallback(MESSAGE, msg->str.c_str());

            } else if (msg->type == ix::WebSocketMessageType::Open)
            {
                cJSON *root;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "connected");
                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECT_SUCCESS, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);

            } else if (msg->type == ix::WebSocketMessageType::Error)
            {
                //A message will be fired when there is an error with the connection. The message type will be ix::WebSocketMessageType::Error.
                // Multiple fields will be available on the event to describe the error.
                cJSON *root, *message;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "error");
                message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "retries", msg->errorInfo.retries);
                cJSON_AddStringToObject(message, "error", msg->errorInfo.reason.c_str());
                cJSON_AddNumberToObject(message, "wait_time", msg->errorInfo.wait_time);
                cJSON_AddNumberToObject(message, "http_status", msg->errorInfo.http_status);
                cJSON_AddItemToObject(root, "message", message);

                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECT_ERROR, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                // The server can send an explicit code and reason for closing.
                // This data can be accessed through the closeInfo object.
                cJSON *root, *message;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "disconnected");
                message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "code", msg->closeInfo.code);
                cJSON_AddStringToObject(message, "reason", msg->closeInfo.reason.c_str());
                cJSON_AddItemToObject(root, "message", message);
                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECTION_DROPPED, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            }
        });

        // Now that our callback is setup, we can start our background thread and receive messages
        webSocket.start();
    }

    static void media_bug_close(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        if(bug) switch_core_media_bug_close(&bug, SWITCH_FALSE);
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if(psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    if (m_initial_meta && strlen(m_initial_meta) > 0) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                                          "sending initial metadata %s\n", m_initial_meta);
                        writeText(m_initial_meta);
                    }
                    m_notify(psession, EVENT_CONNECT, message);
                    break;
                case CONNECTION_DROPPED:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                    m_notify(psession, EVENT_DISCONNECT, message);
                    break;
                case CONNECT_ERROR:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, message);

                    media_bug_close(psession);

                    break;
                case MESSAGE:
                    if(!m_suppress_log)
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "response: %s\n", message);
                    if(processAudio(psession, message) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, message);
                    }
                    break;
            }
            switch_core_session_rwunlock(psession);
        }
    }

    switch_bool_t processAudio(switch_core_session_t* session, const char* message) {
    cJSON* json = cJSON_Parse(message);
    switch_bool_t status = SWITCH_FALSE;
    if (!json) {
        return status;
    }
    const char* event = cJSON_GetObjectCstr(json, "event");
    if (strcmp(event, "clear") == 0) {
    // Log the reception of the clear event
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Received clear event, pausing stream playback\n");
    // Attempt to pause the audio stream
        if (do_pauseresume(session, 1) != SWITCH_STATUS_SUCCESS) {
            // If pausing fails, log an error
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to pause audio stream on clear event\n");
        } else {
            // If pausing is successful, set status to true
            status = SWITCH_TRUE;
        }
    }


    if (strcmp(event, "media") == 0) {
        cJSON* media = cJSON_GetObjectItem(json, "media");
        if (media) {
            cJSON* payload = cJSON_GetObjectItem(media, "payload");
            if (payload && payload->valuestring != nullptr) {
                std::string encodedAudio = payload->valuestring;
                std::string rawAudio = base64_decode(encodedAudio);

                char filePath[256];
                switch_snprintf(filePath, 256, "%s%s%s_%d.tmp.raw", SWITCH_GLOBAL_dirs.temp_dir,
                                SWITCH_PATH_SEPARATOR, m_sessionId.c_str(), m_playFile++);
                std::ofstream fstream(filePath, std::ofstream::binary);
                fstream << rawAudio;
                fstream.close();
                playBackFiles.insert(filePath);

                cJSON* jsonFile = cJSON_CreateString(filePath);
                cJSON_AddItemToObject(media, "file", jsonFile);

                char* jsonString = cJSON_PrintUnformatted(media);
                m_notify(session, EVENT_PLAY, jsonString);
                free(jsonString);
                status = SWITCH_TRUE;
            }
        }
    }
    cJSON_Delete(json);
    return status;
}

    ~AudioStreamer()= default;

    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        webSocket.stop();
    }

    bool isConnected() {
        return (webSocket.getReadyState() == ix::ReadyState::Open);
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if(!this->isConnected()) return;
        webSocket.sendBinary( ix::IXWebSocketSendData((char *)buffer, len) );
    }

    void writeText(const char* text) {
        if(!this->isConnected()) return;
        webSocket.sendUtf8Text(ix::IXWebSocketSendData(text, strlen(text)));
    }

    void deleteFiles() {
        if(m_playFile >0) {
            for (const auto &fileName: playBackFiles) {
                remove(fileName.c_str());
            }
        }
    }

private:
    std::string m_sessionId;
    responseHandler_t m_notify;
    ix::WebSocket webSocket;
    const char* m_initial_meta;
    bool m_suppress_log;
    bool m_global_trace;
    int m_playFile;
    std::unordered_set<std::string> playBackFiles;
};


namespace {
    bool sentAlready = false;
    std::mutex prevMsgMutex;
    std::string prevMsg;

    switch_bool_t filter_json_string(switch_core_session_t *session, const char* message) {
        switch_bool_t send = SWITCH_FALSE;
        cJSON* json = cJSON_Parse(message);
        if (!json) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "parse - failed parsing incoming msg as JSON: %s\n", message);
            return send;
        }

        const cJSON *partial = cJSON_GetObjectItem(json, "partial");

        if(cJSON_IsString(partial)) {
            std::string currentMsg = partial->valuestring;
            prevMsgMutex.lock();
            if(currentMsg == prevMsg) {
                if(!sentAlready) {send = SWITCH_TRUE; sentAlready = true;}
            } else {
                prevMsg = currentMsg; send = SWITCH_TRUE;
            }
            prevMsgMutex.unlock();
        } else {
            send = SWITCH_TRUE;
        }
        cJSON_Delete(json);
        return send;
    }

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool globalTrace, bool suppressLog, int rtp_packets)
    {
        int err; //speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI);
        tech_pvt->sampling = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        tech_pvt->audio_paused = 0;

        if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);

        //size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PERIOD * BUFFERED_SEC);
        size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);

        auto* as = new AudioStreamer(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat, metadata, globalTrace, suppressLog);

        tech_pvt->pAudioStreamer = static_cast<void *>(as);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

        try {
            size_t adjSize = 1; //adjust the buffer size to the closest pow2 size
            while(adjSize < buflen) {
                adjSize *=2;
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: initializing buffer(%zu) to adjusted %zu bytes\n",
                              tech_pvt->sessionId, buflen, adjSize);
            tech_pvt->data = (uint8_t *) switch_core_alloc(pool, adjSize);
            tech_pvt->buffer = (RingBuffer *) switch_core_alloc(pool, sizeof(RingBuffer));
            ringBufferInit(tech_pvt->buffer, tech_pvt->data, adjSize);
        } catch (std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing buffer: %s.\n",
                              tech_pvt->sessionId, e.what());
            return SWITCH_STATUS_FALSE;
        }

        if (desiredSampling != sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        }
        else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
        if (tech_pvt->pAudioStreamer) {
            auto* as = (AudioStreamer *) tech_pvt->pAudioStreamer;
            delete as;
            tech_pvt->pAudioStreamer = nullptr;
        }
    }

    void finish(private_t* tech_pvt) {
        std::shared_ptr<AudioStreamer> aStreamer;
        aStreamer.reset((AudioStreamer *)tech_pvt->pAudioStreamer);
        tech_pvt->pAudioStreamer = nullptr;

        std::thread t([aStreamer]{
            aStreamer->disconnect();
        });
        t.detach();
    }

}

extern "C" {
    switch_status_t do_pauseresume(switch_core_session_t *session, int pause);
    int validate_ws_uri(const char* url, char* wsUri) {
        const char* scheme = nullptr;
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            scheme = "ws";
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            scheme = "wss";
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(*hostEnd) && *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(*portStart)) {
                    return 0;
                }
                ++portStart;
            }
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_text failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;
        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (pAudioStreamer && text) pAudioStreamer->writeText(text);

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        tech_pvt->audio_paused = pause;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        void **ppUserData)
    {
        int deflate, heart_beat;
        bool globalTrace = false;
        bool suppressLog = false;
        const char* buffer_size;
        int rtp_packets = 1;

        switch_channel_t *channel = switch_core_session_get_channel(session);

        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_GLOBAL_TRACE")) {
            globalTrace = true;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value <= INT_MAX && value >= INT_MIN) {
                heart_beat = (int) value;
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if(bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if(bSize >= 20){
                rtp_packets = bSize/20;
            }
        }

        // allocate per-session tech_pvt
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }
        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, channels, metadata, responseHandler, deflate, heart_beat,
                                                        globalTrace, suppressLog, rtp_packets)) {
            destroy_tech_pvt(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

        // Function to generate a random streamSid
    static std::string generateStreamSid() {
        const std::string characters =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::string streamSid = "MZ";
        
        std::random_device rd;  // Obtain a random number from hardware
        std::mt19937 gen(rd()); // Seed the generator
        std::uniform_int_distribution<> distr(0, characters.size() - 1);

        for (int i = 0; i < 32; ++i) {
            streamSid += characters[distr(gen)]; // Use `distr(gen)` instead of `rand()`
        }
        return streamSid;
    }
    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            if (!tech_pvt || tech_pvt->audio_paused) return SWITCH_TRUE;

            if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {

                if (!tech_pvt->pAudioStreamer) {
                    switch_mutex_unlock(tech_pvt->mutex);
                    return SWITCH_TRUE;
                }

                auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

                if(!pAudioStreamer->isConnected()) {
                    switch_mutex_unlock(tech_pvt->mutex);
                    return SWITCH_TRUE;
                }

                // Generate a dynamic streamSid
                std::string streamSid = generateStreamSid();

                if (nullptr == tech_pvt->resampler) {
                    uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                    switch_frame_t frame = {};
                    frame.data = data;
                    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                    size_t available = ringBufferFreeSpace(tech_pvt->buffer);
                    while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                        if(frame.datalen) {
                            // Encode the audio data in base64
                            std::string base64_audio = base64_encode(static_cast<const unsigned char*>(frame.data), frame.datalen);

                            // Construct the JSON message in the required format
                            cJSON* jsonMessage = cJSON_CreateObject();
                            cJSON_AddStringToObject(jsonMessage, "event", "media");
                            cJSON_AddStringToObject(jsonMessage, "streamSid", streamSid.c_str());
                            cJSON* jsonMedia = cJSON_CreateObject();
                            cJSON_AddStringToObject(jsonMedia, "payload", base64_audio.c_str());
                            cJSON_AddItemToObject(jsonMessage, "media", jsonMedia);

                            // Send the JSON message to the WebSocket
                            char* jsonString = cJSON_PrintUnformatted(jsonMessage);
                            pAudioStreamer->writeText(jsonString);
                            free(jsonString);
                            cJSON_Delete(jsonMessage);
                        }
                    }
                } else {
                    uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                    switch_frame_t frame = {};
                    frame.data = data;
                    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                    size_t available = ringBufferFreeSpace(tech_pvt->buffer);

                    while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                        if(frame.datalen) {
                            spx_uint32_t in_len = frame.samples;
                            spx_uint32_t out_len = available >> 1;
                            spx_int16_t out[available];

                            speex_resampler_process_interleaved_int(tech_pvt->resampler,
                                    (const spx_int16_t *)frame.data,
                                    (spx_uint32_t *) &in_len,
                                    &out[0],
                                    &out_len);

                            size_t remaining = 0;

                            if(out_len > 0) {
                                size_t bytes_written = out_len << tech_pvt->channels;
                                // Encode the resampled audio data in base64
                                std::string base64_audio = base64_encode(reinterpret_cast<const unsigned char*>(out), bytes_written);

                                // Construct the JSON message in the required format
                                cJSON* jsonMessage = cJSON_CreateObject();
                                cJSON_AddStringToObject(jsonMessage, "event", "media");
                                cJSON_AddStringToObject(jsonMessage, "streamSid", streamSid.c_str());
                                cJSON* jsonMedia = cJSON_CreateObject();
                                cJSON_AddStringToObject(jsonMedia, "payload", base64_audio.c_str());
                                cJSON_AddItemToObject(jsonMessage, "media", jsonMedia);

                                // Send the JSON message to the WebSocket
                                char* jsonString = cJSON_PrintUnformatted(jsonMessage);
                                pAudioStreamer->writeText(jsonString);
                                free(jsonString);
                                cJSON_Delete(jsonMessage);
                            }
                        }
                    }
                }
                switch_mutex_unlock(tech_pvt->mutex);
            }
            return SWITCH_TRUE;
        }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if(bug)
        {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            char sessionId[MAX_SESSION_ID];
            strcpy(sessionId, tech_pvt->sessionId);

            switch_mutex_lock(tech_pvt->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }

            auto* audioStreamer = (AudioStreamer *) tech_pvt->pAudioStreamer;
            if(audioStreamer) {
                audioStreamer->deleteFiles();
                if (text) audioStreamer->writeText(text);
                finish(tech_pvt);
            }

            destroy_tech_pvt(tech_pvt);

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no bug - websocket connection already closed\n");
        return SWITCH_STATUS_FALSE;
    }
}

