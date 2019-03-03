// This a minimal fully functional example for setting up a server written in C++ that communicates
// with clients via WebRTC data channels. This uses WebSockets to perform the WebRTC handshake
// (offer/accept SDP) with the client. We only use WebSockets for the initial handshake because TCP
// often presents too much latency in the context of real-time action games. WebRTC data channels,
// on the other hand, allow for unreliable and unordered message sending via SCTP.
//
// Author: brian@brkho.com
//         ceztko@gmail.com

#include "observers.h"

#include <webrtc/api/peerconnectioninterface.h>
#include <webrtc/pc/peerconnectionfactory.h>
#include <webrtc/base/physicalsocketserver.h>
#include <webrtc/base/fakenetwork.h>
#include <webrtc/base/ssladapter.h>
#include <webrtc/base/thread.h>
#include <webrtc/p2p/client/basicportallocator.h>
#include "webrtc/p2p/base/basicpacketsocketfactory.h"
#include "webrtc/api/audio_codecs/builtin_audio_decoder_factory.h"
#include "webrtc/api/audio_codecs/builtin_audio_encoder_factory.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <webrtc/common_video/libyuv/include/webrtc_libyuv.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <thread>

// WebSocket++ types are gnarly.
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;
using json = nlohmann::json;

namespace std {
  template<typename T, typename... Args>
  std::unique_ptr<T> make_unique(Args &&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
  }
}

using namespace std;

typedef websocketpp::server<websocketpp::config::asio> WebSocketServer;
typedef WebSocketServer::message_ptr message_ptr;

// The WebSocket server being used to handshake with the clients.
WebSocketServer ws_server;
// The peer conncetion factory that sets up signaling and worker threads. It is also used to create
// the PeerConnection.
rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory;
// The separate thread where all of the WebRTC code runs since we use the main thread for the
// WebSocket listening loop.
std::thread webrtc_thread;
// The WebSocket connection handler that uniquely identifies one of the connections that the
// WebSocket has open. If you want to have multiple connections, you will need to store more than
// one of these.
websocketpp::connection_hdl websocket_connection_handler;
// The peer connection through which we engage in the SDP handshake.
rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection;
// The data channel used to communicate.
rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel;
std::unique_ptr<rtc::BasicPacketSocketFactory> socket_factory;
std::unique_ptr<rtc::Thread> network_thread;
std::unique_ptr<rtc::Thread> worker_thread;
rtc::BasicNetworkManager network_manager;
rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track;

class VideoSink : public rtc::VideoSinkInterface<webrtc::VideoFrame>
{
public:
  void OnFrame(const webrtc::VideoFrame &frame) override;
};

class PeerConnectionObserverImpl : public PeerConnectionObserver
{
public:
  void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
};

class DataChannelObserverImpl : public DataChannelObserver
{
public:
  void OnMessage(const webrtc::DataBuffer& buffer) override;
};

class CreateSessionDescriptionObserverImpl : public CreateSessionDescriptionObserver
{
public:
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
};

// The observer that responds to peer connection events.
PeerConnectionObserverImpl peer_connection_observer;
// The observer that responds to data channel events.
DataChannelObserverImpl data_channel_observer;
// The observer that responds to session description creation events.
CreateSessionDescriptionObserverImpl create_session_description_observer;
// The observer that responds to session description set events. We don't really use this one here.
SetSessionDescriptionObserver set_session_description_observer;

VideoSink video_sink;

// Callback for when the STUN server responds with the ICE candidates.
void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface *candidate)
{
  std::string candidate_str;
  candidate->ToString(&candidate_str);
  json message_object;
  message_object["messageType"] = "candidate";
  json message_payload;
  message_payload["candidate"] = candidate_str;
  message_payload["sdpMid"] = candidate->sdp_mid();
  message_payload["sdpMLineIndex"] = candidate->sdp_mline_index();
  message_object["payload"] = message_payload;
  ws_server.send(websocket_connection_handler, message_object.dump(), websocketpp::frame::opcode::value::text);
}

void PeerConnectionObserverImpl::OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream)
{
  video_track = stream->GetVideoTracks()[0];
  rtc::VideoSinkWants wants;
  video_track->AddOrUpdateSink(&video_sink, wants);
}

// Callback for when the server receives a message on the data channel.
void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer & buffer)
{
  cout << "OnDataChannelMessage" << endl;
  cout << string((char *)buffer.data.data(), buffer.data.size()) << endl;
  webrtc::DataBuffer answer("PONG");
  data_channel->Send(answer);
}

// Callback for when the answer is created. This sends the answer back to the client.
void CreateSessionDescriptionObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface * desc)
{
  peer_connection->SetLocalDescription(&set_session_description_observer, desc);
  std::string offer_string;
  desc->ToString(&offer_string);
  json message_object;
  message_object["messageType"] = "answer";
  json message_payload;
  message_payload["type"] = "answer";
  message_payload["sdp"] = offer_string;
  message_object["payload"] = message_payload;
  ws_server.send(websocket_connection_handler, message_object.dump(), websocketpp::frame::opcode::value::text);
}

// Callback for when the WebSocket server receives a message from the client.
void OnWebSocketMessage(WebSocketServer* s, websocketpp::connection_hdl hdl, message_ptr msg)
{
  websocket_connection_handler = hdl;
  json message_object = json::parse(msg->get_payload());
  std::string type = message_object["messageType"];
  if (type == "offer")
  {
    std::string sdp = message_object["payload"]["sdp"];
    webrtc::PeerConnectionInterface::RTCConfiguration configuration;
    // NOTE: Two stun servers, with two different ip addresses binded
    // to different tcp ports are needed in the general case
    // https://stackoverflow.com/questions/7594390/why-a-stun-server-needs-two-different-public-ip-addresses
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun1.l.google.com:19302";
    configuration.servers.push_back(ice_server);
    ice_server.uri = "stun:stun2.l.google.com:19305";
    configuration.servers.push_back(ice_server);
    auto allocator = std::make_unique<cricket::BasicPortAllocator>(&network_manager, socket_factory.get());
    peer_connection = peer_connection_factory->CreatePeerConnection(configuration, std::move(allocator), nullptr,
                                                                    &peer_connection_observer);

    webrtc::SdpParseError error;
    webrtc::SessionDescriptionInterface* session_description(
        webrtc::CreateSessionDescription("offer", sdp, &error));
    peer_connection->SetRemoteDescription(&set_session_description_observer, session_description);
    peer_connection->CreateAnswer(&create_session_description_observer, nullptr);
  } else if (type == "candidate") {
    std::string candidate = message_object["payload"]["candidate"];
    std::string sdp_mid = message_object["payload"]["sdpMid"];
    int sdp_mline_index = message_object["payload"]["sdpMLineIndex"];
    cout << candidate << ", sdpMid " << sdp_mid << ", sdpMLineIndex " << sdp_mline_index << endl;

    webrtc::SdpParseError error;
    auto candidate_object = webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate, &error);
    if (error.description.length() != 0)
      cout << "ERROR CreateIceCandidate" << error.description << endl;
    if (!peer_connection->AddIceCandidate(candidate_object))
      cout << "ERROR AddIceCandidate" << error.description << endl;
  }
  else
  {
    std::cout << "Unrecognized WebSocket message type." << std::endl;
  }
}

// The thread entry point for the WebRTC thread. This sets the WebRTC thread as the signaling thread
// and creates a worker thread in the background.
void SignalThreadEntry()
{
  // Create the PeerConnectionFactory.
  rtc::InitializeSSL();

  network_thread.reset(rtc::Thread::CreateWithSocketServer().release());
  worker_thread.reset(rtc::Thread::Create().release());
  rtc::Thread *signaling_thread = rtc::Thread::Current();
  network_thread->Start();
  worker_thread->Start();
  peer_connection_factory = webrtc::CreatePeerConnectionFactory(network_thread.get(), worker_thread.get(), signaling_thread,
                                                                nullptr,
                                                                webrtc::CreateBuiltinAudioEncoderFactory(),
                                                                webrtc::CreateBuiltinAudioDecoderFactory(),
                                                                nullptr, nullptr);

  socket_factory.reset(new rtc::BasicPacketSocketFactory(network_thread.get()));
  signaling_thread->Run();
}

// Callback for when the data channel is successfully created. We need to re-register the updated
// data channel here.
void PeerConnectionObserverImpl::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
  cout << "OnDataChannel" << endl;
  data_channel = channel;
  data_channel->RegisterObserver(&data_channel_observer);
}

int i = 0;
void VideoSink::OnFrame(const webrtc::VideoFrame & frame)
{
  auto buffer = frame.video_frame_buffer();
  FILE *fp;
  string filename = "frame" + i;
  fp = fopen((string("D:\\") + filename + ".raw").c_str(), "w");
  webrtc::PrintVideoFrame(*buffer, fp);
  fclose(fp);
  i++;
}

// Main entry point of the code.
int main()
{
  auto ep = boost::asio::ip::tcp::endpoint(
      boost::asio::ip::address::from_string("0.0.0.0"),
      8080
  );


  rtc::LogMessage::LogToDebug(rtc::LoggingSeverity::LS_ERROR);
  webrtc_thread = std::thread(SignalThreadEntry);
  // In a real game server, you would run the WebSocket server as a separate thread so your main
  // process can handle the game loop.
  try {
    ws_server.set_message_handler(bind(OnWebSocketMessage, &ws_server, ::_1, ::_2));
    ws_server.init_asio();
    ws_server.clear_access_channels(websocketpp::log::alevel::all);
    ws_server.set_reuse_addr(true);
    ws_server.listen(ep);
    ws_server.start_accept();
    // I don't do it here, but you should gracefully handle closing the connection.
    ws_server.run();
  } catch(std::exception& e){
    std::cerr << "exception " << e.what() << std::endl;
  }


  rtc::CleanupSSL();
}