`tone.flac`, `tone.ogg` and `tone.mp3` encode an original 0.1-second, 440 Hz mono sine wave at 44,100 Hz, generated for this project's decoder tests. The source amplitude is 16,000 in signed 16-bit PCM. They are covered by the project licence.

`tone_stereo.ogg` encodes original 0.2-second sine waves at 440 Hz (left) and 880 Hz (right), at 44,100 Hz. It verifies that writing planar stereo PCM interleaves both channels without changing their sample values. It is covered by the project licence.

`localhost.crt` and `localhost.key` form a self-signed certificate for loopback HTTPS tests. The private key is public test data and must never be used outside tests.
