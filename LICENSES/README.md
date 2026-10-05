The project licence is in `../LICENSE`.

`librespot.txt` preserves the MIT notice for the upstream librespot implementation used as the protocol and behavioural reference, including adapted identifier conversion and audio decryption logic. It identifies the upstream work, rather than the authorship of this C++ repository.

The Protobuf schemas are copied from the pinned upstream revision; the Login5, client-token and extended-metadata flows hashcash algorithm, Zeroconf pairing cryptography, and Connect/Dealer flows follow that reference. Generated C++ remains a private implementation detail.

Dependencies fetched during configuration retain their own licences in their source distributions.

`shannon.txt` preserves the MIT notice for the Rust Shannon implementation used to port and cross-check the access-point cipher.

The base64 implementation was supplied from VoxelStorm's shared public libraries courtesy of Armchair Software.
