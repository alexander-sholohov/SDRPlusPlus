# SDR++ (sannysanoff fork), The bloat-free SDR software<br>

Please see [upstream project page](https://github.com/AlexandreRouma/SDRPlusPlus) for the main list of its features.

This fork has several goals:

* to add Hermes Lite 2 support
* to experiment with various NOISE REDUCTION algorithms 
* to maintain compatibility with upstream project (regular merges from upstream)
* to add various features that are not in the upstream and have hard time being merged there due to various reasons.

## Installing the fork

Note: Fork may or may not work alongside the original project at present moment. In any case, try. On Windows, should be simpler to run it alongside the original project.

go to the  [recent builds page](https://github.com/sannysanoff/SDRPlusPlus/actions/workflows/build_all.yml), find topmost green build,
click on it, then go to the bottom of the page and find artifact for your system (Linux, Windows, MacOS). It looks like this:
![Example of artifact](https://i.imgur.com/iq8t0Fa.png)

Please note you must be logged in to GitHub to be able to download the artifacts.

## Features compared to original project:


* Hermes Lite 2 support (receive only)
* Noise reduction to benefit SSB/AM - wideband and audio frequency. Wideband is visible on the waterfall. Can turn on both. ***Logmmse*** algorithm is used.
* Mouse wheel scrolling of sliders
* Unicode support in fonts, filenames and installation path (UTF-8), on Windows, too.
* Saving of zoom parameter between sessions
* SNR meter charted below SNR meter - good for comparing antennas
* noise floor calculation differs from original.
* simultaneous multiple audio devices support
* ability to output sound on left or right channel only for particular audio device.
* Airspy HF+ Discovery - narrowing of baseband to hide attenuated parts.
* waterfall drawing is greatly optimized for CPU and GPU bandwidth, important for 4K monitors. 

## Version log (oldest first)


2022.02.19 initial release. 

* Audio AGC controllable via slider (note: default/original is aggressive) - later original author reimplemented it in a different way 
* Interface scaling - later original author reimplemented it in a different way
* Batch of things from features list (above)

2022.02.20

* for Airspy HF+ Discovery, added Fill-In option which cuts off attenuated far sides of the spectrum.
* added secondary audio stream, so you can output same radio to multiple audio cards / virtual cables
* added single-channel (left/right) option for output audio

2022.06.05

* merging from upstream. Project is still on hold because war in my village (grid locator KO80ce).

2022.09.06

* by this time, merge of upstream has been already completed. The log reducion has been greatly optimized compared to previous version.
* you need to enable plugins to use most of fork functionality: noise_reduction_logmmse and hl2_source
* improved waterfall performance

## Feedback

``Have an issue? Works worse than original? File an [issue](https://github.com/sannysanoff/SDRPlusPlus/issues).

Good luck.

## Thanks

Thanks and due respect to original author. 
