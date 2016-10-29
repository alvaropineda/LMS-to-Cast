/*
 *  Squeeze2cast - LMS to Cast gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdarg.h>

#include "squeezedefs.h"
#include "squeeze2cast.h"
#include "util_common.h"
#include "log_util.h"
#include "util.h"
#include "castcore.h"
#include "castitf.h"


#define BLOCKING_SOCKET
#define SELECT_SOCKET

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static SSL_CTX *glSSLctx;
static void *CastSocketThread(void *args);
static void *CastPingThread(void *args);

extern log_level cast_loglevel;
static log_level *loglevel = &cast_loglevel;


#define DEFAULT_RECEIVER	"CC1AD845"


/*----------------------------------------------------------------------------*/
#if OSX
static void set_nosigpipe(sockfd s) {
	int set = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
}
#else
#define set_nosigpipe(s)
#endif


/*----------------------------------------------------------------------------*/
static void set_nonblock(sockfd s) {
#if WIN
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}


/*----------------------------------------------------------------------------*/
#ifdef BLOCKING_SOCKET
static void set_block(sockfd s) {
#if WIN
	u_long iMode = 0;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags & (~O_NONBLOCK));
#endif
}
#endif


/*----------------------------------------------------------------------------*/
static int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout) {
	fd_set w, e;
	struct timeval tval;

	if (connect(sock, addr, addrlen) < 0) {
#if !WIN
		if (last_error() != EINPROGRESS) {
#else
		if (last_error() != WSAEWOULDBLOCK) {
#endif
			return -1;
		}
	}

	FD_ZERO(&w);
	FD_SET(sock, &w);
	e = w;
	tval.tv_sec = timeout;
	tval.tv_usec = 0;

	// only return 0 if w set and sock error is zero, otherwise return error code
	if (select(sock + 1, NULL, &w, &e, timeout ? &tval : NULL) == 1 && FD_ISSET(sock, &w)) {
		int	error = 0;
		socklen_t len = sizeof(error);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &len);
		return error;
	}

	return -1;
}


/*----------------------------------------------------------------------------*/
void InitSSL(void)
{
	const SSL_METHOD *method;

	// initialize SSL stuff
	// SSL_load_error_strings();
	SSL_library_init();

	// build the SSL objects...
	method = SSLv23_client_method();

	glSSLctx = SSL_CTX_new(method);
	SSL_CTX_set_options(glSSLctx, SSL_OP_NO_SSLv2);

}


/*----------------------------------------------------------------------------*/
void EndSSL(void)
{
	SSL_CTX_free(glSSLctx);
}


/*----------------------------------------------------------------------------*/
void swap32(u32_t *n)
{
#if SL_LITTLE_ENDIAN
	u32_t buf = *n;
	*n = 	(((u8_t) (buf >> 24))) +
		(((u8_t) (buf >> 16)) << 8) +
		(((u8_t) (buf >> 8)) << 16) +
		(((u8_t) (buf)) << 24);
#else
#endif
}


/*----------------------------------------------------------------------------*/
bool read_bytes(SSL *ssl, void *buffer, u16_t bytes)
{
	u16_t read = 0;
	sockfd sock = SSL_get_fd(ssl);

	if (sock == -1) return false;

	while (bytes - read) {
		int nb;
#ifdef SELECT_SOCKET
		fd_set rfds;
		struct timeval timeout = { 0, 100000 };
		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		if (select(sock + 1, &rfds, NULL, NULL, &timeout) == -1) {
			LOG_WARN("[s-%p]: socket closed", ssl);
			return false;
		}

		if (!FD_ISSET(sock, &rfds)) continue;

		ERR_clear_error();
		nb = SSL_read(ssl, (u8_t*) buffer + read, bytes - read);
		if (nb > 0) read += nb;
		if (nb < 0) {
			int err = SSL_get_error(ssl, nb);
			LOG_WARN("[s-%p]: SSL error code %d (err:%d)", ssl, err, ERR_get_error());
#ifdef BLOCKING_SOCKET
			return false;
#else
			switch (err) {
				case SSL_ERROR_ZERO_RETURN:
				case SSL_ERROR_SSL:
				case SSL_ERROR_SYSCALL: return false;
				default: break;
			}
#endif
		}
#else
		ERR_clear_error();
		nb = SSL_read(ssl, (u8_t*) buffer + read, bytes - read);
		if (nb < 0) return false;
		read += nb;
#endif
	}

	return true;
}

/*----------------------------------------------------------------------------*/
bool write_bytes(SSL *ssl, void *buffer, u16_t bytes)
{
#ifdef BLOCKING_SOCKET
	return SSL_write(ssl, buffer, bytes) > 0;
#else
	sockfd sock = SSL_get_fd(ssl);

	while (1) {
		int nb;
		fd_set wfds;
		struct timeval timeout = { 0, 100000 };
		FD_ZERO(&wfds);
		FD_SET(sock, &wfds);

		if (select(sock + 1, NULL, &wfds, NULL, &timeout) == -1) return false;
		if (!FD_ISSET(sock, &wfds)) continue;

		ERR_clear_error();
		nb = SSL_write(ssl, buffer, bytes);
		if (nb == bytes) return true;
		switch (SSL_get_error(ssl, nb)) {
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE: continue;
			default: return false;
		}
	}
#endif
}


/*----------------------------------------------------------------------------*/
bool SendCastMessage(SSL *ssl, char *ns, char *dest, char *payload, ...)
{
	CastMessage message = CastMessage_init_default;
	pb_ostream_t stream;
	u8_t *buffer;
	u16_t buffer_len = 4096;
	bool status;
	u32_t len;
	va_list args;

	if (!ssl) return false;

	va_start(args, payload);

	if (dest) strcpy(message.destination_id, dest);
	strcpy(message.namespace, ns);
	len = vsprintf(message.payload_utf8, payload, args);
	message.has_payload_utf8 = true;
	if ((buffer = malloc(buffer_len)) == NULL) return false;
	stream = pb_ostream_from_buffer(buffer, buffer_len);
	status = pb_encode(&stream, CastMessage_fields, &message);
	len = stream.bytes_written;
	swap32(&len);

	status &= write_bytes(ssl, &len, 4);
	status &= write_bytes(ssl, buffer, stream.bytes_written);

	free(buffer);

	if (!stristr(message.payload_utf8, "PING")) {
		LOG_DEBUG("[%p]: Cast sending: %s", ssl, message.payload_utf8);
	}

	return status;
}


/*----------------------------------------------------------------------------*/
bool DecodeCastMessage(u8_t *buffer, u16_t len, CastMessage *msg)
{
	bool status;
	CastMessage message = CastMessage_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(buffer, len);

	status = pb_decode(&stream, CastMessage_fields, &message);
	memcpy(msg, &message, sizeof(CastMessage));
	return status;
}


/*----------------------------------------------------------------------------*/
bool GetNextMessage(SSL *ssl, CastMessage *message)
{
	bool status;
	u32_t len;
	u8_t *buf;

	// the SSL might just have been closed by another thread
	if (!ssl || !read_bytes(ssl, &len, 4)) return false;

	swap32(&len);
	if ((buf = malloc(len))== NULL) return false;
	status = read_bytes(ssl, buf, len);
	status &= DecodeCastMessage(buf, len, message);
	free(buf);
	return status;
}


/*----------------------------------------------------------------------------*/
json_t *GetTimedEvent(void *p, u32_t msWait)
{
	json_t *data;
	tCastCtx *Ctx = (tCastCtx*) p;

	pthread_mutex_lock(&Ctx->eventMutex);
	pthread_cond_reltimedwait(&Ctx->eventCond, &Ctx->eventMutex, msWait);
	data = QueueExtract(&Ctx->eventQueue);
	pthread_mutex_unlock(&Ctx->eventMutex);

	return data;
}


/*----------------------------------------------------------------------------*/
bool ConnectReceiver(tCastCtx *Ctx)
{
	pthread_mutex_lock(&Ctx->Mutex);

	// cannot connect if SSL connection is lost
	if (!Ctx->sslConnect) {
		pthread_mutex_unlock(&Ctx->Mutex);
		return false;
	}

	// already connected, all good
	if (Ctx->Connect == CAST_CONNECTED) {
		pthread_mutex_unlock(&Ctx->Mutex);
		return true;
	}

	Ctx->Connect = CAST_CONNECTING;
	Ctx->waitId = Ctx->reqId++;

	SendCastMessage(Ctx->ssl, CAST_RECEIVER, NULL, "{\"type\":\"LAUNCH\",\"requestId\":%d,\"appId\":\"%s\"}", Ctx->waitId, DEFAULT_RECEIVER);
	pthread_mutex_unlock(&Ctx->Mutex);

	return true;
}


/*----------------------------------------------------------------------------*/
static bool CastConnect(tCastCtx *Ctx)
{
	int err;
	struct sockaddr_in addr;

	// do nothing if we are already connected
	if(!Ctx->ssl) Ctx->ssl  = SSL_new(glSSLctx);
	Ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(Ctx->sock);
	set_nosigpipe(Ctx->sock);

	addr.sin_family = AF_INET;
#if WIN
	addr.sin_addr.s_addr = Ctx->ip.S_un.S_addr;
#else
	addr.sin_addr.s_addr = Ctx->ip.s_addr;
#endif
	addr.sin_port = htons(Ctx->port);

	err = connect_timeout(Ctx->sock, (struct sockaddr *) &addr, sizeof(addr), 2);

	if (err) {
		closesocket(Ctx->sock);
		LOG_ERROR("[%p]: Cannot open socket connection (%d)", Ctx->owner, err);
		return false;
	}

#ifdef BLOCKING_SOCKET
	set_block(Ctx->sock);
#endif
	SSL_set_fd(Ctx->ssl, Ctx->sock);

	if (SSL_connect(Ctx->ssl)) {
		LOG_INFO("[%p]: SSL connection opened [%p]", Ctx->owner, Ctx->ssl);
	}
	else {
		err = SSL_get_error(Ctx->ssl,err);
		LOG_ERROR("[%p]: Cannot open SSL connection (%d)", Ctx->owner, err);
		return false;
	}

	pthread_mutex_lock(&Ctx->Mutex);
	Ctx->sslConnect = true;
	Ctx->Connect = CAST_IDLE;
	Ctx->lastPong = gettime_ms();
	SendCastMessage(Ctx->ssl, CAST_CONNECTION, NULL, "{\"type\":\"CONNECT\"}");
	pthread_mutex_unlock(&Ctx->Mutex);

	return true;
}


/*----------------------------------------------------------------------------*/
static void CastDisconnect(tCastCtx *Ctx, bool disc)
{
	pthread_mutex_lock(&Ctx->Mutex);

	Ctx->reqId = 1;
	Ctx->waitId = Ctx->mediaSessionId = 0;
	Ctx->Connect = CAST_IDLE;
	NFREE(Ctx->sessionId);
	NFREE(Ctx->transportId);
	QueueFlush(&Ctx->eventQueue);
	CastQueueFlush(&Ctx->reqQueue);
	if (disc && Ctx->ssl) {
		SSL_shutdown(Ctx->ssl);
#if 0
		// FIXME: causes a segfault !
		SSL_free(Ctx->ssl);
#endif
		Ctx->ssl = NULL;
		closesocket(Ctx->sock);
		Ctx->sslConnect = false;
	}

	pthread_mutex_unlock(&Ctx->Mutex);
}


/*----------------------------------------------------------------------------*/
void SetMediaVolume(tCastCtx *Ctx, u8_t Volume)
{
	if (Volume > 100) Volume = 100;

	SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
						"{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"mediaSessionId\":%d,\"volume\":{\"level\":%lf,\"muted\":false}}",
						Ctx->reqId++, Ctx->mediaSessionId, (double) Volume / 100.0);
}


/*----------------------------------------------------------------------------*/
void *StartCastDevice(void *owner, bool group, struct in_addr ip, u16_t port, u8_t MediaVolume)
{
	tCastCtx *Ctx = malloc(sizeof(tCastCtx));
	pthread_mutexattr_t mutexAttr;

	Ctx->reqId 		= 1;
	Ctx->waitId 	= Ctx->mediaSessionId = 0;
	Ctx->sessionId 	= Ctx->transportId = NULL;
	Ctx->owner 		= owner;
	Ctx->ssl 		= NULL;
	Ctx->sslConnect = false;
	Ctx->Connect 	= CAST_IDLE;
	Ctx->ip 		= ip;
	Ctx->port		= port;
	Ctx->MediaVolume  = MediaVolume;
	Ctx->group 		= group;

	QueueInit(&Ctx->eventQueue);
	QueueInit(&Ctx->reqQueue);
	pthread_mutexattr_init(&mutexAttr);
	pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&Ctx->Mutex, &mutexAttr);
	pthread_mutexattr_destroy(&mutexAttr);
	pthread_mutex_init(&Ctx->eventMutex, 0);
	pthread_cond_init(&Ctx->eventCond, 0);

	CastConnect(Ctx);

	pthread_create(&Ctx->Thread, NULL, &CastSocketThread, Ctx);
	pthread_create(&Ctx->PingThread, NULL, &CastPingThread, Ctx);

	return Ctx;
}


/*----------------------------------------------------------------------------*/
void UpdateCastDevice(void *p, struct in_addr ip, u16_t port)
{
	tCastCtx *Ctx = p;

	if (Ctx->port != port || Ctx->ip.s_addr != ip.s_addr) {
		LOG_INFO("[%p]: changed ip:port %s:%d", Ctx, inet_ntoa(ip), port);
		pthread_mutex_lock(&Ctx->Mutex);
		Ctx->ip	= ip;
		Ctx->port = port;
		pthread_mutex_unlock(&Ctx->Mutex);
		/*
		Cast disconnection must be done here as the cast thread is likely, but
		not 100% sure, in the re-connect loop, with the increasing retry timer.
		But reconnection is surely done by the cast thread. Note the the call
		below will set SSL to NULL, so cast thread must be carefull with that
		*/
		CastDisconnect(Ctx, true);
	}
}


/*----------------------------------------------------------------------------*/
void StopCastDevice(void *p)
{
	tCastCtx *Ctx = p;

	pthread_mutex_lock(&Ctx->Mutex);
	Ctx->running = false;
	CastDisconnect(Ctx, true);
	pthread_mutex_unlock(&Ctx->Mutex);
	pthread_join(Ctx->PingThread, NULL);
	pthread_join(Ctx->Thread, NULL);
	pthread_cond_destroy(&Ctx->eventCond);
	pthread_mutex_destroy(&Ctx->eventMutex);
	LOG_INFO("[%p]: Cast device stopped", Ctx->owner);
	free(Ctx);
}


/*----------------------------------------------------------------------------*/
void CastQueueFlush(tQueue *Queue)
{
	tReqItem *item;

	while ((item = QueueExtract(Queue)) != NULL) {
		if (!strcasecmp(item->Type,"LOAD")) json_decref(item->data.msg);
		free(item);
	}
}


/*----------------------------------------------------------------------------*/
void ProcessQueue(tCastCtx *Ctx) {
	tReqItem *item;

	if ((item = QueueExtract(&Ctx->reqQueue)) == NULL) return;

	if (!strcasecmp(item->Type, "PLAY")) {
		if (Ctx->mediaSessionId) {
			Ctx->waitId = Ctx->reqId++;

			SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId,
							"{\"type\":\"PLAY\",\"requestId\":%d,\"mediaSessionId\":%d}",
							Ctx->waitId, Ctx->mediaSessionId);
		}
		else {
			LOG_WARN("[%p]: PLAY un-queued but no media session", Ctx->owner);
		}
	}

	if (!strcasecmp(item->Type, "LOAD")) {
		json_t *msg = item->data.msg;
		char *str;

		if (Ctx->Connect == CAST_CONNECTED) {
			Ctx->waitId = Ctx->reqId++;
			Ctx->waitMedia = Ctx->waitId;
			Ctx->mediaSessionId = 0;

			msg = json_pack("{ss,si,ss,sf,sb,so}", "type", "LOAD",
							"requestId", Ctx->waitId, "sessionId", Ctx->sessionId,
							"currentTime", 0.0, "autoplay", 0,
							"media", msg);

			str = json_dumps(msg, JSON_ENCODE_ANY | JSON_INDENT(1));
			SendCastMessage(Ctx->ssl, CAST_MEDIA, Ctx->transportId, str);
			NFREE(str);
		}
		else {
			LOG_WARN("[%p]: LOAD un-queued but not connected", Ctx->owner);
		}

		json_decref(msg);
   }

   free(item);
}


/*----------------------------------------------------------------------------*/
static void *CastPingThread(void *args)
{
	tCastCtx *Ctx = (tCastCtx*) args;
	u32_t last = gettime_ms();

	Ctx->running = true;

	while (Ctx->running) {
		u32_t now = gettime_ms();

		if (now - last > 3000) {
			pthread_mutex_lock(&Ctx->Mutex);

			// ping SSL connection
			if (Ctx->sslConnect) {
				SendCastMessage(Ctx->ssl, CAST_BEAT, NULL, "{\"type\":\"PING\"}");
				if (now - Ctx->lastPong > 15000) {
					LOG_INFO("[%p]: No response to ping", Ctx);
					CastDisconnect(Ctx, true);
				}
			}

			// then ping RECEIVER connection
			if (Ctx->Connect == CAST_CONNECTED) SendCastMessage(Ctx->ssl, CAST_BEAT, Ctx->transportId, "{\"type\":\"PING\"}");

			pthread_mutex_unlock(&Ctx->Mutex);
			last = now;
		}
		usleep(50000);
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static void *CastSocketThread(void *args)
{
	tCastCtx *Ctx = (tCastCtx*) args;
	CastMessage Message;
	json_t *root, *val;
	json_error_t  error;

	Ctx->running = true;

	while (Ctx->running) {
		int requestId = 0;
		bool forward = true;
		const char *str = NULL;

		// this SSL access is not mutex protected, but it should be fine
		if (!GetNextMessage(Ctx->ssl, &Message)) {
			int interval = 100;
			LOG_WARN("[%p]: SSL connection closed", Ctx);
			CastDisconnect(Ctx, true);
			usleep(100000);
			while (Ctx->running && !CastConnect(Ctx)) {
				usleep(interval * 1000);
				if (interval < 5000) interval *= 2;
			}
			continue;
		}

		root = json_loads(Message.payload_utf8, 0, &error);
		val = json_object_get(root, "requestId");

		if (json_is_integer(val)) requestId = json_integer_value(val);
		val = json_object_get(root, "type");

		if (json_is_string(val)) {
			pthread_mutex_lock(&Ctx->Mutex);
			str = json_string_value(val);

			if (!strcasecmp(str, "MEDIA_STATUS")) {
				LOG_DEBUG("[%p]: type:%s (id:%d) %s", Ctx->owner, str, requestId, GetMediaItem_S(root, 0, "playerState"));
			}
			else {
				LOG_DEBUG("[%p]: type:%s (id:%d)", Ctx->owner, str, requestId);
			}

			LOG_SDEBUG("(s:%s) (d:%s)\n%s", Message.source_id, Message.destination_id, Message.payload_utf8);

			// Connection closed by peer
			if (!strcasecmp(str,"CLOSE")) {
				CastDisconnect(Ctx, false);
			}

			// respond to device ping
			if (!strcasecmp(str,"PING")) {
				SendCastMessage(Ctx->ssl, CAST_BEAT, Message.source_id, "{\"type\":\"PONG\"}");
				json_decref(root);
				forward = false;
			}

			// respond to device ping
			if (!strcasecmp(str,"PONG")) {
				Ctx->lastPong = gettime_ms();
				json_decref(root);
				forward = false;
			}

			if (Ctx->waitId && Ctx->waitId <= requestId) {

				// receiver status before connection is fully established
				if (!strcasecmp(str,"RECEIVER_STATUS") && Ctx->Connect == CAST_CONNECTING) {
					const char *str;

					NFREE(Ctx->sessionId);
					str = GetAppIdItem(root, DEFAULT_RECEIVER, "sessionId");
					if (str) Ctx->sessionId = strdup(str);
					NFREE(Ctx->transportId);
					str = GetAppIdItem(root, DEFAULT_RECEIVER, "transportId");
					if (str) Ctx->transportId = strdup(str);

					if (Ctx->sessionId && Ctx->transportId) {
						Ctx->Connect = CAST_CONNECTED;
						LOG_INFO("[%p]: Receiver launched", Ctx->owner);
						SendCastMessage(Ctx->ssl, CAST_CONNECTION, Ctx->transportId,
									"{\"type\":\"CONNECT\",\"origin\":{}}");
					}

					json_decref(root);
					forward = false;
				}

				// media status only acquired for expected id
				if (!strcasecmp(str,"MEDIA_STATUS") && Ctx->waitMedia == requestId) {
					int id = GetMediaItem_I(root, 0, "mediaSessionId");
					if (id) {
						Ctx->waitMedia = 0;
						Ctx->mediaSessionId = id;
						LOG_INFO("[%p]: Media session id %d", Ctx->owner, Ctx->mediaSessionId);
						// set media volume when session is re-connected
						SetMediaVolume(Ctx, Ctx->MediaVolume);
					}
					// Don't need to forward this, no valuable info
					forward = false;
				}

				// must be done at the end, once all parameters have been acquired
				Ctx->waitId = 0;
				if (Ctx->Connect == CAST_CONNECTED) ProcessQueue(Ctx);

			}

			pthread_mutex_unlock(&Ctx->Mutex);
		}

		// queue event and signal handler
		if (forward) {
			pthread_mutex_lock(&Ctx->eventMutex);
			QueueInsert(&Ctx->eventQueue, root);
			pthread_cond_signal(&Ctx->eventCond);
			pthread_mutex_unlock(&Ctx->eventMutex);
		}
	}

	return NULL;
}





