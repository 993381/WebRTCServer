#define _WINSOCKAPI_ 
#include "internal/videorenderer.h"

/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** PeerConnectionManager.cpp
**
** -------------------------------------------------------------------------*/

#include <iostream>
#include <fstream>
#include <utility>

#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/test/fakeconstraints.h"
#include "media/engine/webrtcvideodecoderfactory.h"
#include <internal/PeerConnectionManager.h>
#include "rtc_base/strings/json.h"
#include "base/optional.h"
#include "internal/CapturerFactory.h"


// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";


/* ---------------------------------------------------------------------------
**  helpers that should be moved somewhere else
** -------------------------------------------------------------------------*/

inline void workProcess()
{
	auto thread = rtc::Thread::Current();
	auto msg_cnt = thread->size();
	RTC_LOG(INFO) << "process message. : last " << msg_cnt;

	while (msg_cnt > 0)
	{
		rtc::Message msg;
		if (!thread->Get(&msg, 0))
			return;
		thread->Dispatch(&msg);
	}
}

#ifdef WIN32
std::string getServerIpFromClientIp(int clientip)
{
	return "127.0.0.1";
}
#else
#include <net/if.h>
#include <ifaddrs.h>
std::string getServerIpFromClientIp(int clientip) {
	std::string serverAddress;
	char host[NI_MAXHOST];
	struct ifaddrs *ifaddr = NULL;
	if (getifaddrs(&ifaddr) == 0)
	{
		for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
		{
			if ((ifa->ifa_netmask != NULL) && (ifa->ifa_netmask->sa_family == AF_INET) && (ifa->ifa_addr != NULL) && (ifa->ifa_addr->sa_family == AF_INET))
			{
				struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
				struct sockaddr_in* mask = (struct sockaddr_in*)ifa->ifa_netmask;
				if ((addr->sin_addr.s_addr & mask->sin_addr.s_addr) == (clientip & mask->sin_addr.s_addr))
				{
					if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0)
					{
						serverAddress = host;
						break;
					}
				}
			}
		}
	}
	freeifaddrs(ifaddr);
	return serverAddress;
}
#endif

struct IceServer
{
	std::string url;
	std::string user;
	std::string pass;
};

IceServer getIceServerFromUrl(const std::string& url, const std::string& clientIp = "")
{
	IceServer srv;
	srv.url = url;

	std::size_t pos = url.find_first_of(':');
	if (pos != std::string::npos)
	{
		std::string protocol = url.substr(0, pos);
		std::string uri = url.substr(pos + 1);
		std::string credentials;

		std::size_t pos = uri.find('@');
		if (pos != std::string::npos)
		{
			credentials = uri.substr(0, pos);
			uri = uri.substr(pos + 1);
		}

		if ((uri.find("0.0.0.0:") == 0) && (clientIp.empty() == false))
		{
			// answer with ip that is on same network as client
			std::string clienturl = getServerIpFromClientIp(inet_addr(clientIp.c_str()));
			clienturl += uri.substr(uri.find_first_of(':'));
			uri = clienturl;
		}
		srv.url = protocol + ":" + uri;

		if (!credentials.empty())
		{
			pos = credentials.find(':');
			if (pos == std::string::npos)
			{
				srv.user = credentials;
			}
			else
			{
				srv.user = credentials.substr(0, pos);
				srv.pass = credentials.substr(pos + 1);
			}
		}
	}

	return srv;
}

/* ---------------------------------------------------------------------------
**  Constructor
** -------------------------------------------------------------------------*/
PeerConnectionManager::PeerConnectionManager(const std::list<std::string>& iceServerList
                                             , const webrtc::AudioDeviceModule::AudioLayer audioLayer
                                             , const std::string& publishFilter)
	: audioDeviceModule_(webrtc::AudioDeviceModule::Create(0, audioLayer))
	  , audioDecoderfactory_(webrtc::CreateBuiltinAudioDecoderFactory())
	  , peer_connection_factory_(webrtc::CreatePeerConnectionFactory(NULL,
	                                                                 NULL,
	                                                                 NULL,
	                                                                 audioDeviceModule_,
	                                                                 webrtc::CreateBuiltinAudioEncoderFactory(),
	                                                                 audioDecoderfactory_,
	                                                                 webrtc::CreateBuiltinVideoEncoderFactory(),
	                                                                 webrtc::CreateBuiltinVideoDecoderFactory(),
	                                                                 NULL, NULL))
	  , iceServerList_(iceServerList)
	  , m_publishFilter(publishFilter)
{
	// build video audio map
	//m_videoaudiomap = getV4l2AlsaMap();
}

/* ---------------------------------------------------------------------------
**  Destructor
** -------------------------------------------------------------------------*/
PeerConnectionManager::~PeerConnectionManager()
{
	std::vector<std::string> peerIds;

	{
		std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
		for (auto pair : this->peer_connectionobs_map_)
		{
			peerIds.push_back(pair.first);
		}
	}
	for (auto & peerId : peerIds)
	{
		this->hangUp(peerId);
	}

	{
		std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
		this->peer_connectionobs_map_.clear();
	}
}


/* ---------------------------------------------------------------------------
**  return deviceList as JSON vector
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getMediaList()
{
	Json::Value value(Json::arrayValue);

	const std::list<std::string> videoCaptureDevice = CapturerFactory::GetVideoCaptureDeviceList(m_publishFilter);
	for (auto videoDevice : videoCaptureDevice)
	{
		Json::Value media;
		media["video"] = videoDevice;

		std::map<std::string, std::string>::iterator it = m_videoaudiomap.find(videoDevice);
		if (it != m_videoaudiomap.end())
		{
			media["audio"] = it->second;
		}
		value.append(media);
	}

	const std::list<std::string> videoList = CapturerFactory::GetVideoSourceList(m_publishFilter);
	for (auto videoSource : videoList)
	{
		Json::Value media;
		media["video"] = videoSource;
		value.append(media);
	}

	/*for (auto url : m_urlVideoList)
	{
		Json::Value media;
		media["video"] = url.first;
		auto audioIt = m_urlAudioList.find(url.first);
		if (audioIt != m_urlAudioList.end())
		{
			media["audio"] = audioIt->first;
		}
		value.append(media);
	}
*/
	return value;
}

/* ---------------------------------------------------------------------------
**  return video device List as JSON vector
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getVideoDeviceList()
{
	Json::Value value(Json::arrayValue);

	const std::list<std::string> videoCaptureDevice = CapturerFactory::GetVideoCaptureDeviceList(m_publishFilter);
	for (auto videoDevice : videoCaptureDevice)
	{
		value.append(videoDevice);
	}

	return value;
}

/* ---------------------------------------------------------------------------
**  return audio device List as JSON vector
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getAudioDeviceList()
{
	Json::Value value(Json::arrayValue);

	if (std::regex_match("audiocap://", m_publishFilter))
	{
		int16_t num_audioDevices = audioDeviceModule_->RecordingDevices();
		RTC_LOG(INFO) << "nb audio devices:" << num_audioDevices;

		for (int i = 0; i < num_audioDevices; ++i)
		{
			char name[webrtc::kAdmMaxDeviceNameSize] = {0};
			char id[webrtc::kAdmMaxGuidSize] = {0};
			if (audioDeviceModule_->RecordingDeviceName(i, name, id) != -1)
			{
				RTC_LOG(INFO) << "audio device name:" << name << " id:" << id;
				value.append(name);
			}
		}
	}

	return value;
}

/* ---------------------------------------------------------------------------
**  return iceServers as JSON vector
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getIceServers(const std::string& clientIp)
{
	Json::Value urls;

	for (auto iceServer : iceServerList_)
	{
		Json::Value server;
		Json::Value urlList(Json::arrayValue);
		IceServer srv = getIceServerFromUrl(iceServer, clientIp);
		RTC_LOG(INFO) << "ICE URL:" << srv.url;
		urlList.append(srv.url);
		server["urls"] = urlList;
		if (srv.user.length() > 0) server["username"] = srv.user;
		if (srv.pass.length() > 0) server["credential"] = srv.pass;
		urls.append(server);
	}

	Json::Value iceServers;
	iceServers["iceServers"] = urls;

	return iceServers;
}

/* ---------------------------------------------------------------------------
**  get PeerConnection associated with peerid
** -------------------------------------------------------------------------*/
rtc::scoped_refptr<webrtc::PeerConnectionInterface> PeerConnectionManager::getPeerConnection(const std::string& peerid)
{
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
	std::map<std::string, PeerConnectionObserver*>::iterator it = peer_connectionobs_map_.find(peerid);
	if (it != peer_connectionobs_map_.end())
	{
		peerConnection = it->second->getPeerConnection();
	}
	return peerConnection;
}

/* ---------------------------------------------------------------------------
**  get PeerConnectionObserver associated with peerid
** -------------------------------------------------------------------------*/
PeerConnectionManager::PeerConnectionObserver* PeerConnectionManager::getPeerConnectionObserver(
	const std::string& peerid)
{
	PeerConnectionManager::PeerConnectionObserver* peerConnection = nullptr;
	auto it = peer_connectionobs_map_.find(peerid);

	if (it != peer_connectionobs_map_.end())
	{
		peerConnection = it->second;
	}
	return peerConnection;
}

/* ---------------------------------------------------------------------------
**  add ICE candidate to a PeerConnection
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::addIceCandidate(const std::string& peerid, const Json::Value& jmessage)
{
	bool result = false;
	std::string sdp_mid;
	int sdp_mlineindex = 0;
	std::string sdp;
	if (!rtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName, &sdp_mid)
		|| !rtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName, &sdp_mlineindex)
		|| !rtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp))
	{
		RTC_LOG(LS_WARNING) << "Can't parse received message:" << jmessage;
	}
	else
	{
		std::unique_ptr<webrtc::IceCandidateInterface> candidate(
			webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, NULL));
		if (!candidate.get())
		{
			RTC_LOG(LS_WARNING) << "Can't parse received candidate message.";
		}
		else
		{
			rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = this->getPeerConnection(peerid);
			if (peerConnection)
			{
				if (!peerConnection->AddIceCandidate(candidate.get()))
				{
					RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
				}
				else
				{
					result = true;
				}
			}
		}
	}
	Json::Value answer;
	/*if (result)
	{
		answer = result;
	}*/
	return answer;
}

PeerConnectionManager::PeerConnectionObserver* PeerConnectionManager::createClientOffer(const std::string& peerid)
{
	RTC_LOG(INFO) << __FUNCTION__ << " peerId:" << peerid;


	PeerConnectionObserver* peerConnectionObserver = this->getPeerConnectionObserver(peerid);
	if (!peerConnectionObserver)
	{
		peerConnectionObserver = this->CreatePeerConnection(peerid);

		if (!peerConnectionObserver)
		{
			RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnection";
		}
		else
		{
			// register peerid
			{
				std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
				peer_connectionobs_map_.insert(
					std::pair<std::string, PeerConnectionObserver*>(peerid, peerConnectionObserver));
			}
		}
	}
	return peerConnectionObserver;
}

/* ---------------------------------------------------------------------------
** create an offer for a call
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::createOffer(const std::string& peerid, 
	const std::string& options, std::shared_ptr<core::queue::ConcurrentQueue<cv::Mat>> i_stack,
	std::function<void(webrtc::SessionDescriptionInterface*)> i_funcOnSucess)
{
	RTC_LOG(INFO) << __FUNCTION__ << " video:" << " options:" << options;
	Json::Value offer;
	PeerConnectionObserver* peerConnectionObserver = this->getPeerConnectionObserver(peerid);
	if (!peerConnectionObserver)
	{
		peerConnectionObserver = this->CreatePeerConnection(peerid);

	}
	if (!peerConnectionObserver)
	{
		RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnection";
	}
	else
	{
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = peerConnectionObserver->
			getPeerConnection();

	
		// register peerid
		{
			std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
			peer_connectionobs_map_.insert(
				std::pair<std::string, PeerConnectionObserver*>(peerid, peerConnectionObserver));
		}
			
		if (!this->AddStreams(peerConnection, options, i_stack))
		{
			RTC_LOG(WARNING) << "Can't add stream";
		}

		// ask to create offer
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtcoptions;
		CreateSessionDescriptionObserver* session_description_observer = CreateSessionDescriptionObserver::Create(peerConnection);
		session_description_observer->setOnSuccess(i_funcOnSucess);
		peerConnection->CreateOffer(session_description_observer, rtcoptions);

	}
	return offer;
}


const Json::Value PeerConnectionManager::joinClientOffer(const std::string& peerid,
                                                         const Json::Value& jmessage,
                                                         std::function<void(webrtc::SessionDescriptionInterface*)>
                                                         i_funcOnSucess)
{
	Json::Value answer;
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = this->getPeerConnection(peerid);

	CreateSessionDescriptionObserver* session_description_observer = CreateSessionDescriptionObserver::Create(
		peerConnection);
	session_description_observer->setOnSuccess(i_funcOnSucess);

	// ask to create offer
	webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtcoptions;
	peerConnection->CreateOffer(CreateSessionDescriptionObserver::Create(peerConnection), rtcoptions);

	// waiting for offer


	return answer;
}

/* ---------------------------------------------------------------------------
** set answer to a call initiated by createOffer
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::createAnswerToClientOffer(const std::string& peerid,
                                                                   const Json::Value& jmessage,
                                                                   std::function<void(
	                                                                   webrtc::SessionDescriptionInterface*)>
                                                                   i_funcOnSucess)
{
	RTC_LOG(INFO) << jmessage;
	Json::Value answer;
	std::string type;
	std::string sdp;
	if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type)
		|| !rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp))
	{
		RTC_LOG(LS_WARNING) << "Can't parse received message.";
	}
	else
	{
		webrtc::SessionDescriptionInterface* session_description(webrtc::CreateSessionDescription(type, sdp, NULL));
		if (!session_description)
		{
			RTC_LOG(LS_WARNING) << "Can't parse received session description message.";
		}
		else
		{
			RTC_LOG(INFO) << "From peerid:" << peerid << " received session description :" << session_description->
				type();

			rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = this->getPeerConnection(peerid);
			if (peerConnection)
			{
				peerConnection->SetRemoteDescription(SetSessionDescriptionObserver::Create(peerConnection),
				                                     session_description);

				// waiting for remote description
				int count = 10;
				while ((peerConnection->remote_description() == NULL) && (--count > 0))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				}
				if (peerConnection->remote_description() == NULL)
				{
					RTC_LOG(LS_WARNING) << "remote_description is NULL";
				}

				// create answer
				webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtcoptions;
				CreateSessionDescriptionObserver* session_description_observer = CreateSessionDescriptionObserver::
					Create(peerConnection);
				session_description_observer->setOnSuccess(i_funcOnSucess);

				rtcoptions.offer_to_receive_video = 1;
				/*	rtcoptions.offer_to_receive_audio = 0;*/
				peerConnection->CreateAnswer(session_description_observer, rtcoptions);

				RTC_LOG(INFO) << "nbStreams local:" << peerConnection->local_streams()->count() << " remote:" <<
					peerConnection
					->
					remote_streams()
					->count()
					<< " localDescription:" << peerConnection->local_description()
					<< " remoteDescription:" << peerConnection->remote_description();

				//// waiting for answer
				//count = 10;
				//while ((peerConnection->local_description() == NULL) && (--count > 0))
				//{
				//	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				//}

				/*	RTC_LOG(INFO) << "nbStreams local:" << peerConnection->local_streams()->count() << " remote:" <<
						peerConnection
						->
						remote_streams()
						->count()
						<< " localDescription:" << peerConnection->local_description()
						<< " remoteDescription:" << peerConnection->remote_description();*/

				// return the answer
				/*const webrtc::SessionDescriptionInterface* desc = peerConnection->local_description();
				if (desc)
				{
					std::string sdp;
					desc->ToString(&sdp);

					answer[kSessionDescriptionTypeName] = desc->type();
					answer[kSessionDescriptionSdpName] = sdp;
				}
				else
				{
					RTC_LOG(LS_ERROR) << "Failed to create answer";
				}*/
			}
		}
	}
	return answer;
}

/* ---------------------------------------------------------------------------
** set answer to a call initiated by createOffer
** -------------------------------------------------------------------------*/
void PeerConnectionManager::setAnswer(const std::string& peerid, const Json::Value& jmessage)
{
	RTC_LOG(INFO) << jmessage;

	std::string type;
	std::string sdp;
	if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type)
		|| !rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp))
	{
		RTC_LOG(LS_WARNING) << "Can't parse received message.";
	}
	else
	{
		webrtc::SessionDescriptionInterface* session_description(webrtc::CreateSessionDescription(type, sdp, NULL));
		if (!session_description)
		{
			RTC_LOG(LS_WARNING) << "Can't parse received session description message.";
		}
		else
		{
			RTC_LOG(INFO) << "From peerid:" << peerid << " received session description :" << session_description->
				type();

			std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
			rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = this->getPeerConnection(peerid);
			if (peerConnection)
			{
				peerConnection->SetRemoteDescription(SetSessionDescriptionObserver::Create(peerConnection),
				                                     session_description);
			}
		}
	}
}

void PeerConnectionManager::startOpenCVStreaming(const std::string& peer_id,
                                                 std::shared_ptr<core::queue::ConcurrentQueue<cv::Mat>> i_stack)
{
	std::string options;
	PeerConnectionObserver* peer_connection_observer = this->getPeerConnectionObserver(peer_id);

	this->AddStreams(peer_connection_observer->getPeerConnection(), options, i_stack);
}

void PeerConnectionManager::stopOpenCVStreaming(const std::string& peer_id)
{
	std::string options;
	PeerConnectionObserver* pcObserver = this->getPeerConnectionObserver(peer_id);

	if (pcObserver)
	{
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = pcObserver->getPeerConnection();

		rtc::scoped_refptr<webrtc::StreamCollectionInterface> localstreams(peerConnection->local_streams());
		for (unsigned int i = 0; i < localstreams->count(); i++)
		{
			auto stream = localstreams->at(i);

			std::string streamLabel = stream->id();
			bool stillUsed = this->streamStillUsed(streamLabel);
			if (!stillUsed)
			{
				RTC_LOG(LS_ERROR) << "hangUp stream is no more used " << streamLabel;
				std::lock_guard<std::mutex> mlock(m_streamMapMutex);
				auto it = stream_map_.find(streamLabel);
				if (it != stream_map_.end())
				{
					stream_map_.erase(it);
				}

				RTC_LOG(LS_ERROR) << "hangUp stream closed " << streamLabel;
			}

			peerConnection->RemoveStream(stream);
		}
	}
}

/* ---------------------------------------------------------------------------
**  auto-answer to a call
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::call(const std::string& peerid, const std::string& options,
                                              const Json::Value& jmessage)
{
	RTC_LOG(INFO) << __FUNCTION__ << " video:" << " options:" << options;

	Json::Value answer;

	std::string type;
	std::string sdp;

	if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type)
		|| !rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp))
	{
		RTC_LOG(LS_WARNING) << "Can't parse received message.";
	}
	else
	{
		PeerConnectionObserver* peerConnectionObserver = this->CreatePeerConnection(peerid);
		if (!peerConnectionObserver)
		{
			RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnectionObserver";
		}
		else if (!peerConnectionObserver->getPeerConnection().get())
		{
			RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnection";
			delete peerConnectionObserver;
		}
		else
		{
			rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = peerConnectionObserver->
				getPeerConnection();
			RTC_LOG(INFO) << "nbStreams local:" << peerConnection->local_streams()->count() << " remote:" <<
				peerConnection
				->
				remote_streams()
				->count() <<
				" localDescription:" << peerConnection->local_description();

			// register peerid
			{
				std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
				peer_connectionobs_map_.insert(
					std::pair<std::string, PeerConnectionObserver*>(peerid, peerConnectionObserver));
			}

			// set remote offer
			webrtc::SessionDescriptionInterface* session_description(webrtc::CreateSessionDescription(type, sdp, NULL));
			if (!session_description)
			{
				RTC_LOG(LS_WARNING) << "Can't parse received session description message.";
			}
			else
			{
				peerConnection->SetRemoteDescription(SetSessionDescriptionObserver::Create(peerConnection),
				                                     session_description);
			}

			// waiting for remote description
			int count = 10;
			while ((peerConnection->remote_description() == NULL) && (--count > 0))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			}
			if (peerConnection->remote_description() == NULL)
			{
				RTC_LOG(LS_WARNING) << "remote_description is NULL";
			}

			// add local stream
			if (!this->AddStreams(peerConnection, options))
			{
				RTC_LOG(LS_WARNING) << "Can't add stream";
			}

			// create answer
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtcoptions;
			rtcoptions.offer_to_receive_video = 0;
			rtcoptions.offer_to_receive_audio = 0;
			peerConnection->CreateAnswer(CreateSessionDescriptionObserver::Create(peerConnection), rtcoptions);

			RTC_LOG(INFO) << "nbStreams local:" << peerConnection->local_streams()->count() << " remote:" <<
				peerConnection
				->
				remote_streams()
				->count()
				<< " localDescription:" << peerConnection->local_description()
				<< " remoteDescription:" << peerConnection->remote_description();

			// waiting for answer
			count = 10;
			while ((peerConnection->local_description() == NULL) && (--count > 0))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			}

			RTC_LOG(INFO) << "nbStreams local:" << peerConnection->local_streams()->count() << " remote:" <<
				peerConnection
				->
				remote_streams()
				->count()
				<< " localDescription:" << peerConnection->local_description()
				<< " remoteDescription:" << peerConnection->remote_description();

			// return the answer
			const webrtc::SessionDescriptionInterface* desc = peerConnection->local_description();
			if (desc)
			{
				std::string sdp;
				desc->ToString(&sdp);

				answer[kSessionDescriptionTypeName] = desc->type();
				answer[kSessionDescriptionSdpName] = sdp;
			}
			else
			{
				RTC_LOG(LS_ERROR) << "Failed to create answer";
			}
		}
	}
	return answer;
}

bool PeerConnectionManager::streamStillUsed(const std::string& streamLabel)
{
	bool stillUsed = false;
	for (auto it : peer_connectionobs_map_)
	{
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = it.second->getPeerConnection();
		rtc::scoped_refptr<webrtc::StreamCollectionInterface> localstreams(peerConnection->local_streams());
		for (unsigned int i = 0; i < localstreams->count(); i++)
		{
			if (localstreams->at(i)->id() == streamLabel)
			{
				stillUsed = true;
				break;
			}
		}
	}
	return stillUsed;
}

/* ---------------------------------------------------------------------------
**  hangup a call
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::hangUp(const std::string& peerid)
{
	bool result = false;
	RTC_LOG(INFO) << __FUNCTION__ << " " << peerid;

	PeerConnectionObserver* pcObserver = NULL;
	{
		std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
		std::map<std::string, PeerConnectionObserver*>::iterator it = peer_connectionobs_map_.find(peerid);
		if (it != peer_connectionobs_map_.end())
		{
			pcObserver = it->second;
			RTC_LOG(INFO) << "Remove PeerConnection peerid:" << peerid;
			peer_connectionobs_map_.erase(it);
		}

		if (pcObserver)
		{
			rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = pcObserver->getPeerConnection();

			rtc::scoped_refptr<webrtc::StreamCollectionInterface> localstreams(peerConnection->local_streams());
			for (unsigned int i = 0; i < localstreams->count(); i++)
			{
				auto stream = localstreams->at(i);

				std::string streamLabel = stream->id();
				bool stillUsed = this->streamStillUsed(streamLabel);
				if (!stillUsed)
				{
					RTC_LOG(LS_ERROR) << "hangUp stream is no more used " << streamLabel;
					std::lock_guard<std::mutex> mlock(m_streamMapMutex);
					std::map<std::string, rtc::scoped_refptr<webrtc::VideoTrackInterface>>::iterator it = stream_map_.
						find(streamLabel);
					if (it != stream_map_.end())
					{
						stream_map_.erase(it);
					}

					RTC_LOG(LS_ERROR) << "hangUp stream closed " << streamLabel;
				}

				peerConnection->RemoveStream(stream);
			}

			delete pcObserver;
			result = true;
		}
	}
	Json::Value answer;
	/*if (result)
	{
		answer = result;
	}*/
	RTC_LOG(INFO) << __FUNCTION__ << " " << peerid << " result:" << result;
	return answer;
}


/* ---------------------------------------------------------------------------
**  get list ICE candidate associayed with a PeerConnection
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getIceCandidateList(const std::string& peerid)
{
	RTC_LOG(INFO) << __FUNCTION__;

	Json::Value value;
	std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
	std::map<std::string, PeerConnectionObserver*>::iterator it = peer_connectionobs_map_.find(peerid);
	if (it != peer_connectionobs_map_.end())
	{
		PeerConnectionObserver* obs = it->second;
		if (obs)
		{
			value = obs->getIceCandidateList();
		}
		else
		{
			RTC_LOG(LS_ERROR) << "No observer for peer:" << peerid;
		}
	}
	return value;
}

/* ---------------------------------------------------------------------------
**  get PeerConnection list
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getPeerConnectionList()
{
	Json::Value value(Json::arrayValue);

	std::lock_guard<std::mutex> peerlock(m_peerMapMutex);
	for (auto it : peer_connectionobs_map_)
	{
		Json::Value content;

		// get local SDP
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection = it.second->getPeerConnection();
		if ((peerConnection) && (peerConnection->local_description()))
		{
			std::string sdp;
			peerConnection->local_description()->ToString(&sdp);
			content["sdp"] = sdp;

			Json::Value streams;
			rtc::scoped_refptr<webrtc::StreamCollectionInterface> localstreams(peerConnection->local_streams());
			if (localstreams)
			{
				for (unsigned int i = 0; i < localstreams->count(); i++)
				{
					if (localstreams->at(i))
					{
						Json::Value tracks;

						const webrtc::VideoTrackVector& videoTracks = localstreams->at(i)->GetVideoTracks();
						for (unsigned int j = 0; j < videoTracks.size(); j++)
						{
							Json::Value track;
							tracks[videoTracks.at(j)->kind()].append(videoTracks.at(j)->id());
						}
						const webrtc::AudioTrackVector& audioTracks = localstreams->at(i)->GetAudioTracks();
						for (unsigned int j = 0; j < audioTracks.size(); j++)
						{
							Json::Value track;
							tracks[audioTracks.at(j)->kind()].append(audioTracks.at(j)->id());
						}

						Json::Value stream;
						stream[localstreams->at(i)->id()] = tracks;

						streams.append(stream);
					}
				}
			}
			content["streams"] = streams;
		}

		// get Stats
		//		content["stats"] = it.second->getStats();

		Json::Value pc;
		pc[it.first] = content;
		value.append(pc);
	}
	return value;
}

/* ---------------------------------------------------------------------------
**  get StreamList list
** -------------------------------------------------------------------------*/
const Json::Value PeerConnectionManager::getStreamList()
{
	std::lock_guard<std::mutex> mlock(m_streamMapMutex);
	Json::Value value(Json::arrayValue);
	for (auto it : stream_map_)
	{
		value.append(it.first);
	}
	return value;
}

/* ---------------------------------------------------------------------------
**  check if factory is initialized
** -------------------------------------------------------------------------*/
bool PeerConnectionManager::InitializePeerConnection()
{
	return (peer_connection_factory_.get() != NULL);
}

/* ---------------------------------------------------------------------------
**  create a new PeerConnection
** -------------------------------------------------------------------------*/
PeerConnectionManager::PeerConnectionObserver* PeerConnectionManager::CreatePeerConnection(const std::string& peerid)
{
	webrtc::PeerConnectionInterface::RTCConfiguration config;

	config.enable_dtls_srtp = true;
	for (auto iceServer : iceServerList_)
	{
		webrtc::PeerConnectionInterface::IceServer server;
		IceServer srv = getIceServerFromUrl(iceServer);
		server.uri = srv.url;
		server.username = srv.user;
		server.password = srv.pass;
		config.servers.push_back(server);
	}

	RTC_LOG(INFO) << __FUNCTION__ << "CreatePeerConnection peerid:" << peerid;
	PeerConnectionObserver* obs = new PeerConnectionObserver(this, peerid, config);
	if (!obs)
	{
		RTC_LOG(LS_ERROR) << __FUNCTION__ << "CreatePeerConnection failed";
	}
	return obs;
}

/* ---------------------------------------------------------------------------
**  get the capturer from its URL
** -------------------------------------------------------------------------*/
rtc::scoped_refptr<webrtc::VideoTrackInterface> PeerConnectionManager::CreateVideoTrack(
	const std::string& videourl, const std::map<std::string, std::string>& opts, std::shared_ptr<core::queue::ConcurrentQueue<cv::Mat>> i_stack)
{
	RTC_LOG(INFO) << "videourl:" << videourl;

	std::unique_ptr<cricket::VideoCapturer> capturer;
	if (videourl == "VideoSender")
	{
		capturer = CapturerFactory::CreateOpenCVCapturer(
			videourl, i_stack);

	}
	else
	{
		capturer = CapturerFactory::CreateVideoCapturer(
			videourl, opts, m_publishFilter);
	}

	rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track;
	if (!capturer)
	{
		RTC_LOG(LS_ERROR) << "Cannot create capturer video:" << videourl;
	}
	else
	{
		webrtc::FakeConstraints constraints;
		std::list<std::string> keyList = {
			webrtc::MediaConstraintsInterface::kMinWidth, webrtc::MediaConstraintsInterface::kMaxWidth,
			webrtc::MediaConstraintsInterface::kMinHeight, webrtc::MediaConstraintsInterface::kMaxHeight,
			webrtc::MediaConstraintsInterface::kMinFrameRate, webrtc::MediaConstraintsInterface::kMaxFrameRate,
			webrtc::MediaConstraintsInterface::kMinAspectRatio, webrtc::MediaConstraintsInterface::kMaxAspectRatio
		};

		for (auto key : keyList)
		{
			if (opts.find(key) != opts.end())
			{
				constraints.AddMandatory(key, opts.at(key));
			}
		}
		rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource = peer_connection_factory_->CreateVideoSource(
			std::move(capturer), &constraints);

		std::string label = videourl;
		label.erase(std::remove_if(label.begin(), label.end(), [](char c)
		            {
			            return c == ' ' || c == ':' || c == '.' || c == '/';
		            })
		            , label.end());

		video_track = peer_connection_factory_->CreateVideoTrack(label, videoSource);
	}
	return video_track;
}

//
//rtc::scoped_refptr<webrtc::AudioTrackInterface> PeerConnectionManager::CreateAudioTrack(const std::string & audiourl, const std::map<std::string, std::string> & opts)
//{
//	RTC_LOG(INFO) << "audiourl:" << audiourl;
//
//	rtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource;
//	if ((audiourl.find("rtsp://") == 0) && (std::regex_match("rtsp://", m_publishFilter)))
//	{
//#ifdef HAVE_LIVE555
//		audioDeviceModule_->Terminate();
//		audioSource = RTSPAudioSource::Create(audioDecoderfactory_, audiourl, opts);
//#endif
//	}
//	else if (std::regex_match("audiocap://", m_publishFilter))
//	{
//		audioDeviceModule_->Init();
//		int16_t num_audioDevices = audioDeviceModule_->RecordingDevices();
//		int16_t idx_audioDevice = -1;
//		for (int i = 0; i < num_audioDevices; ++i)
//		{
//			char name[webrtc::kAdmMaxDeviceNameSize] = { 0 };
//			char id[webrtc::kAdmMaxGuidSize] = { 0 };
//			if (audioDeviceModule_->RecordingDeviceName(i, name, id) != -1)
//			{
//				if (audiourl == name)
//				{
//					idx_audioDevice = i;
//					break;
//				}
//			}
//		}
//		RTC_LOG(LS_ERROR) << "audiourl:" << audiourl << " idx_audioDevice:" << idx_audioDevice;
//		if ((idx_audioDevice >= 0) && (idx_audioDevice < num_audioDevices))
//		{
//			audioDeviceModule_->SetRecordingDevice(idx_audioDevice);
//			cricket::AudioOptions opt;
//			audioSource = peer_connection_factory_->CreateAudioSource(opt);
//		}
//	}
//
//	rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track;
//	if (!audioSource) {
//		RTC_LOG(LS_ERROR) << "Cannot create capturer audio:" << audiourl;
//	}
//	else {
//		std::string label = audiourl + "_audio";
//		label.erase(std::remove_if(label.begin(), label.end(), [](char c) { return c == ' ' || c == ':' || c == '.' || c == '/'; })
//			, label.end());
//		audio_track = peer_connection_factory_->CreateAudioTrack(label, audioSource);
//	}
//
//	return audio_track;
//}

bool PeerConnectionManager::AddStreams(webrtc::PeerConnectionInterface* peer_connection, const std::string& options)
{
	std::shared_ptr<core::queue::ConcurrentQueue<cv::Mat>> i_stack;

	return AddStreams(peer_connection, options, i_stack);
}

/* ---------------------------------------------------------------------------
**  Add a stream to a PeerConnection
** -------------------------------------------------------------------------*/
bool PeerConnectionManager::AddStreams(webrtc::PeerConnectionInterface* peer_connection, const std::string& options,
                                       std::shared_ptr<core::queue::ConcurrentQueue<cv::Mat>> i_stack)
{
	bool ret = false;

	// look in urlmap
	std::string video = "VideoSender";
	//auto videoit = m_urlVideoList.find(video);
	//if (videoit != m_urlVideoList.end()) {
	//	video = videoit->second;
	//}
	/*std::string audio = audiourl;
	auto audioit = m_urlAudioList.find(audio);
	if (audioit != m_urlAudioList.end()) {
		audio = audioit->second;
	}
*/
	// compute stream label removing space because SDP use label
	std::string streamLabel = video + "|" + options;
	streamLabel.erase(std::remove_if(streamLabel.begin(), streamLabel.end(),
	                                 [](char c) { return c == ' ' || c == ':' || c == '.' || c == '/'; })
	                  , streamLabel.end());

	bool existingStream = false;
	{
		std::lock_guard<std::mutex> mlock(m_streamMapMutex);
		existingStream = (stream_map_.find(streamLabel) != stream_map_.end());
	}

	if (!existingStream)
	{
		// compute audiourl if not set
		/*if (audio.empty()) {
			if (video.find("rtsp://") == 0) {
				audio = video;
			}
			else {
				std::map<std::string, std::string>::iterator it = m_videoaudiomap.find(video);
				if (it != m_videoaudiomap.end()) {
					audio = it->second;
				}
			}
		}*/

		// convert options string into map
		std::istringstream is(options);
		std::map<std::string, std::string> opts;
		std::string key, value;
		while (std::getline(std::getline(is, key, '='), value, '&'))
		{
			opts[key] = value;
		}

		// set bandwidth
		if (opts.find("bitrate") != opts.end())
		{
			int bitrate = std::stoi(opts.at("bitrate"));

			webrtc::PeerConnectionInterface::BitrateParameters bitrateParam;
			bitrateParam.min_bitrate_bps = absl::optional<int>(bitrate / 2);
			bitrateParam.current_bitrate_bps = absl::optional<int>(bitrate);
			bitrateParam.max_bitrate_bps = absl::optional<int>(bitrate * 2);
			peer_connection->SetBitrate(bitrateParam);

			RTC_LOG(LS_WARNING) << "set bitrate:" << bitrate;
		}

		// need to create the stream
		rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(this->CreateVideoTrack(video, opts, i_stack));
		/*	rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(this->CreateAudioTrack(audio, opts));*/
		RTC_LOG(INFO) << "Adding Stream to map";
		std::lock_guard<std::mutex> mlock(m_streamMapMutex);
		stream_map_[streamLabel] = video_track;
	}


	{
		std::lock_guard<std::mutex> mlock(m_streamMapMutex);
		std::map<std::string, rtc::scoped_refptr<webrtc::VideoTrackInterface>>::iterator it = stream_map_.find(
			streamLabel);
		if (it != stream_map_.end())
		{
			rtc::scoped_refptr<webrtc::MediaStreamInterface> stream = peer_connection_factory_->CreateLocalMediaStream(
				streamLabel);
			if (!stream.get())
			{
				RTC_LOG(LS_ERROR) << "Cannot create stream";
			}
			else
			{
				//std::pair < rtc::scoped_refptr<webrtc::VideoTrackInterface>, rtc::scoped_refptr<webrtc::AudioTrackInterface> > pair = it->second;
				rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(it->second);
				if ((video_track) && (!stream->AddTrack(video_track)))
				{
					RTC_LOG(LS_ERROR) << "Adding VideoTrack to MediaStream failed";
				}

				/*rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(pair.second);
				if ((audio_track) && (!stream->AddTrack(audio_track)))
				{
					RTC_LOG(LS_ERROR) << "Adding AudioTrack to MediaStream failed";
				}
*/
				if (!peer_connection->AddStream(stream))
				{
					RTC_LOG(LS_ERROR) << "Adding stream to PeerConnection failed";
				}
				else
				{
					RTC_LOG(INFO) << "stream added to PeerConnection";
					ret = true;
				}
			}
		}
		else
		{
			RTC_LOG(LS_ERROR) << "Cannot find stream";
		}
	}

	return ret;
}

void PeerConnectionManager::PeerConnectionObserver::SetOnIceCandidate(
	std::function<void(const webrtc::IceCandidateInterface*)> i_funcOnIceCandidate)
{
	funcOnIceCandidate = i_funcOnIceCandidate;
}

/* ---------------------------------------------------------------------------
**  ICE callback
** -------------------------------------------------------------------------*/
void PeerConnectionManager::PeerConnectionObserver::OnIceCandidate(const webrtc::IceCandidateInterface* candidate)
{
	RTC_LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
	if (funcOnIceCandidate)
	{
		funcOnIceCandidate(candidate);
	}
	else
	{
		std::string sdp;
		if (!candidate->ToString(&sdp))
		{
			RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
		}
		else
		{
			RTC_LOG(INFO) << sdp;

			Json::Value jmessage;
			jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
			jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
			jmessage[kCandidateSdpName] = sdp;
			iceCandidateList_.append(jmessage);
		}
	}
}
