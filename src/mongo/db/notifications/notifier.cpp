//
//  Mongodb update notifier in C++
//  Binds PUB socket to tcp://*:5556
//
//  Meng Zhang <mzhang@yottaa.com>
//
#include "pch.h"
#include "notifier.hpp"
#include <zmq.hpp>

#define STOP_TOKEN "stop"

namespace mongo{

	MongodbChangeNotifier* MongodbChangeNotifier::Instance()
	{

		static MongodbChangeNotifier notifier;
		return &notifier;
	}

	MongodbChangeNotifier::MongodbChangeNotifier()
		:context_(1),worker_(context_,ZMQ_PULL),manager_(context_,ZMQ_PULL),publisher_(context_,ZMQ_PUB),started_(0),stopped_(0)
	{

	}

	MongodbChangeNotifier::~MongodbChangeNotifier()
	{

	}
	
	void MongodbChangeNotifier::stop()
	{
		if(!started_)
	    		return;
		stopped_ = 1;

		zmq::socket_t producer(context_, ZMQ_PUSH);
		producer.connect("inproc://manager");
		zmq::message_t message(1);
		memcpy(message.data(),"S",1);
		producer.send(message);

	}

	void MongodbChangeNotifier::start(const std::string& proto)
	{
		if(proto.empty())
			return;

		worker_.bind("inproc://worker");
		manager_.bind("inproc://manager");

		publisher_.bind(proto.c_str());
		publisher_.bind("ipc://mongo_change_notifier.ipc");

		thread_ = boost::thread(&MongodbChangeNotifier::pump, this);

		started_ = 1;
	}

	void MongodbChangeNotifier::postNotification(const op operation, const std::string& ns, const BSONObj& id,const BSONObj& update)
	{
		if(!started_)
			return;

		zmq::socket_t producer(context_, ZMQ_PUSH);
		producer.connect("inproc://worker");

		BSONObjBuilder b;
		b.append("ns",ns);
		b.append(id["_id"]);		
		b.append("op",(int)operation);
		b.append("change",update);
		std::string msg = b.obj().jsonString(Strict,0);
		zmq::message_t message(msg.size());
		memcpy(message.data(),msg.data(),msg.size());
		producer.send(message);

	}


	void MongodbChangeNotifier::pump (void) 
	{
		//  Initialize poll set
		zmq::pollitem_t items [] = {
			{ worker_, 0, ZMQ_POLLIN, 0 },
			{ manager_, 0, ZMQ_POLLIN, 0 },
		};
		while (!stopped_) {
			try{
				zmq::message_t message;
				zmq::poll(&items[0],2,-1);

				if(items[1].revents & ZMQ_POLLIN){
					manager_.recv(&message);
					break;
				}

				if(items[0].revents & ZMQ_POLLIN){
					worker_.recv(&message);
					log() << "[notifier]\t"<< std::string(static_cast<char*>(message.data()),message.size()) <<std::endl;
					//  Send message to all subscribers
					publisher_.send(message);
				}
			}catch(std::exception& e){
				log() << "[notifier]\t exception caught " << e.what() << std::endl;
			}
		}
    
	}

}
