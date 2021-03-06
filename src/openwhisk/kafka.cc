#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <ebbrt/Cpu.h>
#include "../SeussController.h"

#include "openwhisk.h"

#include "cppkafka/configuration.h"
#include "cppkafka/consumer.h"
#include "cppkafka/group_information.h"
#include "cppkafka/metadata.h"
#include "cppkafka/producer.h"
#include "cppkafka/topic.h"

using std::exception;
using std::string;
using std::cout;
using std::endl;
using std::vector;

namespace po = boost::program_options;

using cppkafka::BrokerMetadata;
using cppkafka::Configuration;
using cppkafka::Consumer;
using cppkafka::Exception;
using cppkafka::GroupInformation;
using cppkafka::GroupMemberInformation;
using cppkafka::MemberAssignmentInformation;
using cppkafka::Message;
using cppkafka::MessageList;
using cppkafka::MessageBuilder;
using cppkafka::Metadata;
using cppkafka::Producer;
using cppkafka::TopicMetadata;
using cppkafka::TopicPartition;
using cppkafka::TopicPartitionList;

namespace {
string kafka_broker;
uint64_t invoker_id = 0;
uint64_t invoker_delay = 0;
Configuration config;
} // end local

void openwhisk::kafka::ping_producer_loop() {
  if (kafka_broker.empty()) {
    std::cerr << "Error: No Kafka broker specified." << std::endl;
    return;
  }

  Producer kafka_producer(config);
  msg::PingMessage ping;
  ping.name_.instance_ = invoker_id;
  cout << "kafka: Sending heartbeat to OpenWhisk at a rate of 1 every "
       << ping_freq_ms << "ms." << endl;
  cout << "Ping msg: " << ping.to_json() << endl;

  // Optional: dump kafka state
  try {
    Metadata metadata = kafka_producer.get_metadata();
    cout << "Kafka brokers: " << endl;
    for (const BrokerMetadata &broker : metadata.get_brokers()) {
      cout << "* " << broker.get_host() << endl;
    }
    cout << endl;
    cout << "Kafka topics: " << endl;
    for (const TopicMetadata &topic : metadata.get_topics()) {
      cout << "* " << topic.get_name() << ": " << topic.get_partitions().size()
           << " partitions" << endl;
    }

    // Fetch the group information
    vector<GroupInformation> groups = [&]() {
      return kafka_producer.get_consumer_groups();
    }();

    if (groups.empty()) {
      cout << "Found no consumers" << endl;
    }
    cout << "Found the following consumers: " << endl;
    for (const GroupInformation &group : groups) {
      cout << "* \"" << group.get_name() << "\" having the following ("
           << group.get_members().size() << ") members: " << endl;
      for (const GroupMemberInformation &info : group.get_members()) {
        cout << "    - " << info.get_member_id() << " @ "
             << info.get_client_host();
        MemberAssignmentInformation assignment(info.get_member_assignment());
        cout << " has assigned: " << assignment.get_topic_partitions();
        cout << endl;
      }
      cout << endl;
    }
  } catch (const Exception &ex) {
    cout << "Error fetching group information: " << ex.what() << endl;
  }

  while (1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ping_freq_ms));
    // Create a heartbeat message
    try {
      MessageBuilder builder("health");
      auto pl = ping.to_json();
      builder.payload(pl);
      kafka_producer.produce(builder);
    } catch (const Exception &ex) {
      cout << "Error fetching group information: " << ex.what() << endl;
    }
  }
}

void openwhisk::kafka::activation_consumer_loop() {
  if (kafka_broker.empty()) {
    std::cerr << "Error: No Kafka broker specified." << std::endl;
    return;
  }

  // Create the invoker topic and consumer
  Consumer kafka_consumer(config);
  Producer kafka_producer(config);
  std::string default_topic = "invoker" + std::to_string(invoker_id);
  cout << "kafka: consumer subscribe to:" << default_topic << endl;
  kafka_consumer.subscribe({default_topic});

  // Stream in Activation messages
  while (1) {
    // Try to consume a message
    Message msg = kafka_consumer.poll();
    if (msg) {
      // If we managed to get a message
      if (msg.get_error()) {
        // Ignore EOF notifications from rdkafka
        if (!msg.is_eof()) {
          cout << "[+] Received error notification: " << msg.get_error()
               << endl;
        }
      } else {
        // Get the message payload
        std::string amjson = msg.get_payload();
        kafka_consumer.async_commit(msg);
        msg::ActivationMessage am(amjson);
        std::time_t result = std::time(nullptr);
        cout << std::asctime(std::localtime(&result)) << result
             << " got activation " << am.transid_.name_ << endl;

        if (openwhisk::mode == "null") {
          // Create an empty response
          msg::CompletionMessage cm(am);
          cm.invoker_.instance_ = invoker_id;
          cm.response_.status_code_ = 0; // success
          MessageBuilder builder("completed0");
          auto pl = cm.to_json();
          builder.payload(pl);
          kafka_producer.produce(builder);
        } else {
          /* For invokerHealthTestActions we immediately return */
          if (am.action_.name_ == "invokerHealthTestAction0") {
            // Create an empty response
            msg::CompletionMessage cm(am);
            cm.invoker_.instance_ = invoker_id;
            cm.response_.status_code_ = 0; // success
            MessageBuilder builder("completed0");
            auto pl = cm.to_json();
            builder.payload(pl);
            kafka_producer.produce(builder);
          } else {
            // Send request to the seuss controller
            auto cmf = seuss::controller->ScheduleActivation(am);
            cmf.Then(
                [&kafka_producer](ebbrt::Future<msg::CompletionMessage> cmf) {
                  auto cm = cmf.Get();
                  cm.invoker_.instance_ = invoker_id;
                  MessageBuilder builder("completed0");
                  auto pl = cm.to_json();
                  builder.payload(pl);
                  kafka_producer.produce(builder);
                });
          }
        } // end if(mode=null)
      }   // end if(msg.get_error())
      if (invoker_delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(invoker_delay));
      }
    }   // end if(msg)
  }     // end while(1)
}

po::options_description openwhisk::kafka::program_options() {
  po::options_description options("Kafka");
  options.add_options()
	("kafka-brokers,k", po::value<string>(), "kafka host")
  ("kafka-topic,t", po::value<uint64_t>(), "invoker Id");
  return options;
}

bool openwhisk::kafka::init(po::variables_map &vm) {
  if (vm.count("kafka-brokers"))
    kafka_broker = vm["kafka-brokers"].as<string>();
  if (vm.count("kafka-topic"))
    invoker_id = vm["kafka-topic"].as<uint64_t>();
  if (vm.count("invoker-delay"))
    invoker_delay = vm["invoker-delay"].as<uint64_t>();

  std::cout << "Kafka: hosts " << kafka_broker << std::endl;
  std::cout << "Kafka: invoker #" << std::to_string(invoker_id) << std::endl;
  if (invoker_delay > 0)
    std::cout << "Kafka: invoker delay " << std::to_string(invoker_delay) << " ms" << std::endl;

  if (kafka_broker.empty()) {
    std::cerr << "Error: No Kafka broker specified." << std::endl;
    return false;
  }

  config = {{"metadata.broker.list", kafka_broker}, {"group.id", invoker_id}};

  /** New producer and topic confiuguration */
  Producer kafka_producer(config);
  std::string default_topic = "invoker" + std::to_string(invoker_id);
  cout << "kafka: create new topic: " << default_topic << endl;
  kafka_producer.get_topic(default_topic);

  return true;
}
