Android side expects a real ADNL client backend library:

- Path per ABI: `app/src/main/jniLibs/<ABI>/libton_adnl_client.so`
- Runtime symbols required:
  - `ton_adnl_client_create(const char* private_key, const char* egress_adnl_address, const char* local_advertise_ipv4)`
    - `local_advertise_ipv4`: real WLAN/LAN IPv4 for `adnl.addressList` (same UDP port as bind). Not `0.0.0.0` / `127.0.0.1`.
  - `ton_adnl_client_start(void* handle)`
  - `ton_adnl_client_stop(void* handle)`
  - `ton_adnl_client_destroy(void* handle)`
  - `ton_adnl_client_send(void* handle, const uint8_t* data, size_t len)`
  - `ton_adnl_client_set_on_message(void* handle, callback, void* user_data)`

To implement `libton_adnl_client.so`, the backend must use TON ADNL stack (not tonlibjson raw API):

- Required TON components/targets:
  - `adnl`
  - `dht`
  - `rldp`
  - `tdactor`
  - `tdutils`
  - `tdnet`
  - `ton_crypto`
  - `tl_api`
  - `keys`
  - `keyring`

- Required headers (minimum):
  - `adnl/adnl-network-manager.h`
  - `adnl/adnl-peer-table.h`
  - `adnl/utils.hpp`
  - `keys/encryptor.h`
  - `td/actor/actor.h`
  - `td/actor/scheduler.h`
  - `td/utils/port/IPAddress.h`

The frame payload passed to `ton_adnl_client_send` and callback must be exactly the protocol frames used by VPS:

- `OPEN`: type(1), streamId(4), hostLen(2), host, port(2), tokenLen(2), token
- `DATA`: type(1), streamId(4), payloadLen(4), payload
- `CLOSE`: type(1), streamId(4)
- `ERROR`: type(1), streamId(4), msgLen(2), message
