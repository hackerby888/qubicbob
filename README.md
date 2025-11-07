### System Requirements:
- cmake and clang (or gcc)
- KeyDB Engine [Check installation guide](KEYDB_INSTALL.md)
- Memory (RAM): 16 GB
- Processor (CPU): 4 Cores (with AVX2 support)
- Storage (Disk): 100 GB Fast SSD / NVMe

Install dependencies and necessary tools to operate bob:
```
sudo apt-get update;
apt install vim net-tools tmux cmake git libjsoncpp-dev build-essential cmake uuid-dev libhiredis-dev zlib1g-dev unzip -y;
```

### BUILD

On Linux, make sure `cmake` and `make` commands are installed and then run:
```
mkdir build;
cd build;
cmake ../;
make;
```

### CONFIGURATION
An example file, `default_config_bob.json`, is provided and contains the minimal configuration required to run bob.

For the trusted-node field, the expected format is `NODE_IP:NODE_PORT:PASSCODE_LOGGING`. If the `PASSCODE_LOGGING` is not available, the simplified format `NODE_IP:NODE_PORT` should be used. 

- Too tight `request-cycle-ms` or `future-offset` may lead to overloading the node.
- `run-server` means opening a server and listening at port `server-port` to serve a few important data (like the core baremetal)
```
{
  "p2p-node": ["bob:23.88.1.189:21842"],
  "request-cycle-ms": 500,
  "request-logging-cycle-ms": 150,
  "future-offset": 3,
  "log-level": "info",
  "keydb-url": "tcp://127.0.0.1:6379",
  "run-server": true,
  "server-port": 21842,
  "arbitrator-identity": "AFZPUAIYVPNUYGJRQVLUKOPPVLHAZQTGLYAAUUNBXFTVTAMSBKQBLEIEPCVJ",
  "trusted-entities": ["QCTBOBEPDEZGBBCSOWGBYCAIZESDMEVRGLWVNBZAPBIZYEJFFZSPPIVGSCVL"],
  "node-seed":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
}
```

### USEFUL RESOURCES
#### Using bob
- [What is logging event in Qubic?](LOGGING_IN_QUBIC.MD)
- [REST API endpoints](REST_API.md)
- [Mastering findlog method](FINDLOG.MD)
- [Dealing with tx and logging in bob](DEAL_WITH_TX.MD)
- Increase kernel buffer size to [improve the stability of lite node](KERN_BUF_SIZE.MD)
#### Inside bob
- [Anatomy of bob](ANATOMY_OF_BOB.MD)
- [Indexer indexing Qubic data](INDEXER_INDEXING_DATA.MD)

### USAGE
`./bob <config_path>`

### INSTALLATION SCRIPTS
All in one batch file for the lazy:
```
apt update && apt upgrade -y;
apt install vim net-tools tmux cmake git libjsoncpp-dev build-essential cmake uuid-dev libhiredis-dev zlib1g-dev unzip -y;
git clone https://github.com/krypdkat/qubicbob.git;
cd qubicbob;
mkdir build;
cd build;
cmake ..;
make bob;
curl -fsSL https://download.keydb.dev/open-source-dist/keyring.gpg | sudo gpg --dearmor -o /usr/share/keyrings/keydb-archive-keyring.gpg;
echo "deb [signed-by=/usr/share/keyrings/keydb-archive-keyring.gpg] https://download.keydb.dev/open-source-dist jammy main" | sudo tee /etc/apt/sources.list.d/keydb.list;
apt update;
apt install keydb
```

