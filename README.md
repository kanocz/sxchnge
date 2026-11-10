# sxchnge
Easy exchange struct and plain data via TCP between golang and C

Protocol providing handshake on connect, AES-CTR encryption, comparing version and list of datatypes, sizes etc

Encryption is enabled by setting `AESKey` (AES-128/192/256 key in hex) to the same value on both sides. The C version uses OpenSSL libcrypto, link your program with `-lcrypto`.
