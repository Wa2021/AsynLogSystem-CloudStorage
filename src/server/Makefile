test:Test.cpp base64.cpp Service.hpp DataManager.hpp Config.hpp Util.hpp
	g++ -o $@ Test.cpp base64.cpp -std=c++17 -lpthread -lstdc++fs -ljsoncpp -lbundle -levent -lssl -lcrypto 
gdb_test:Test.cpp
	g++ -g -o $@ $^ -std=c++17 -lpthread -lstdc++fs -ljsoncpp  -lbundle -levent
.PHONY:clean
clean:
	rm -rf test gdb_test ./deep_storage ./low_storage ./logfile storage.data
