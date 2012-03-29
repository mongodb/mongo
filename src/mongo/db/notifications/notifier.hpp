
#pragma once

#include "../../pch.h"
#include "../jsobj.h"
#include <zmq.hpp>
#include <boost/thread.hpp>

#define NOTIFY_INSERTION(ns,newObj)				\
	do{											\
		BSONElement e;							\
		if(newObj.getObjectID(e))				\
		{										\
			BSONObjBuilder b;					\
			b.append(e);												\
			MongodbChangeNotifier::Instance()->postNotification(INSERT,ns,b.done(),newObj);	\
		}																\
				}while(0)

#define NOTIFY_DELETION(ns,rloc)				\
	do{											\
		BSONElement e;											\
		if( BSONObj( rloc.rec() ).getObjectID( e ) ) {			\
			BSONObjBuilder b;									\
			b.append( e );												\
			MongodbChangeNotifier::Instance()->postNotification(DELETE,ns,b.done(),BSONObj()); \
		}																\
			}while(0)						   

namespace mongo{

	typedef enum _op { INSERT = 0, UPDATE = 1, DELETE = 2} op;

	class MongodbChangeNotifier : private boost::noncopyable {

	public:
		static MongodbChangeNotifier* Instance();
    
		void start(const std::string& proto);
		void stop(void);
		void postNotification(const op operation, const std::string& ns, const BSONObj& id,const BSONObj& update);

	protected:
		MongodbChangeNotifier();
		~MongodbChangeNotifier();
		void pump(void);


	private:
		zmq::context_t context_;
		zmq::socket_t worker_;
		zmq::socket_t manager_;
		zmq::socket_t publisher_;
    
		boost::thread thread_;
		int started_;
		int stopped_;
	};

}
