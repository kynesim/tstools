===
AC3
===

Specification taken from a_52a.pdf

I *think* that looking at the start of `syncinfo` should give me all the
information I need to (a) determine if this is AC3 and (b) determine if it is
"main" audio.

::

 syncframe:                     -- synchronization frame
    syncinfo:                   -- synchronization info
        16: syncword            -- 0x0B77
        16: crc1
         2: fscod               -- [1]
         6: frmsizecod
    bsi:                        -- bit stream info
         5: bsid                -- 8 in *this* version of the standard [2]
         3: bsmod               -- [3]
         3: acmod               -- which main service channels are in use [4]
         2: cmixlev             -- center mix level (if 3 front channels)
         2: surmixlev           -- surround mix level (if surround sound channel)
         2: dsurmod             -- dolby surround mode (if in 2/0 mode) 2=surround
         1: lfeon
         5: dialnorm
         1: compre
            <etc>
    for n in range(6):
        audblk:                 -- coded audio block
            (256 new audio samples per channel)
    auxdata:
    errorcheck:                 -- CRC for whole syncframe
         1: crcsv
        16: crc2


 [1] fscod: sampling rate in kHz:
                00 = 48
                01 = 44.1
                10 = 32
                11 = reserved

 [2] bsid: values less than 8 are for subsets of the standard. If the software
     can decode data with bsid=8, it can also decode data with bsid<8.

 [3] bit stream mode:

    (the "full" column gives the full service flag for use in DVB's
    AC-3_descriptor:AC-3_type [5])

        bsmod   acmod   type of service                                 full?
        000     any     main audio service: complete main (CM)          1
        001     any     main audio service: music & effects (ME)        0
        010     any     associated service: visually impaired (VI)      X
        011     any     associated service: hearing impaired (HI)       X
        100     any     associated service: dialogue (D)                0
        101     any     associated service: commentary (C)              X
        110     any     associated service: emergency (E)               1
        111     001     associated service: voice over (VO)             0
        111     010-111 main audio service: karaoke                     1

 [4] audio coding mode:

        bit     meaning
        0       center channel in use
        1       <too complex to summarise here>
        2       surround sound channels in use

                audio   full
                coding  bandwidth
        acmod   mode    chans   order
        0       1+1     2       Ch1,Ch2         ("dual mono")
        1       1/0     1       C
        2       2/0     2       L,R
        3       3/0     3       L,C,R
        4       2/1     3       L,R,S
        5       3/1     4       L,C,R,S
        6       2/2     4       L,R,SL,SR
        7       3/2     5       L,C,R,SL,SR

AC3 in TS
=========

AC sync frame contains 1536 audio samples. Its duration is::

        48kHz   -> 32ms
        44.1kHz -> approx 34.83ms
        32 kHz  -> 48ms

For ATSC::

        stream_type     0x81
        stream_id       0xBD (private_stream_1) in PES header

        registration_descriptor:  (in PMT)
             8: descriptor_tag          -- 0x05
             8: descriptor_length       -- 0x04
            32: format_identifier       -- 0x41432D33 ("AC-3")

        audio_stream_descriptor:  (in PSI)
             8: descriptor_tag          -- 0x81
             8: descriptor_length       -- <number of bytes after this field>
             3: sample_rate_code
             5: bsid                    -- same as bsid above [2]
             6: bit_rate_code
             2: surround_mode
             3: bsmod                   -- same as bsmod above [3]
             4: num_channels
             1: full_svc
            ---------------- further optional fields, depending on the above

        ISO_639_language_code descriptor allows a stream to be tagged with
        the 24-bit ISO 639 language code.

For DVB::

        stream_type     0x06 (private_data)
        stream_id       0xBD (private_stream_1) in PES header

        AC-3_descriptor:        (in PSI and PMT)
             8: descriptor_tag          -- 0x6A
             8: descriptor_length       -- <number of bytes after this field>
             1: AC-3_type_flag
             1: bsid_flag
             1: mainid_flag
             1: asvc_flag
             4: <reserved bits, set to 0>
            ---------------- further fields present if their flag is set
             8: AC-3_type               -- [5]
             8: bsid                    -- same as bsid above [2]
             8: mainid                  -- 0-7 main audio service id
             8: asvc                    -- associate service with main service
            ---------------- further info to the number of bytes indicated
           n*8: additional info

 [5] I *think* this is interpreted as follows:

        Bits    Meaning
        0-2     0: mono
                1: 1+1
                2: 2 channel (stereo)
                3: 2 channel Dolby surround encoded (stereo)
                4: Multichannel audio (>2 channels)
                Other values reserved
        3-5     same as [3], bit stream mode
        6       0: Use channel in combination with another
                1: Full service channel, use alone
        7       Must be 0

.. ***** BEGIN LICENSE BLOCK *****

License
-------
Version: MPL 1.1

The contents of this file are subject to the Mozilla Public License Version
1.1 (the "License"); you may not use this file except in compliance with
the License. You may obtain a copy of the License at
http://www.mozilla.org/MPL/

Software distributed under the License is distributed on an "AS IS" basis,
WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
for the specific language governing rights and limitations under the
License.

The Original Code is the MPEG TS, PS and ES tools.

The Initial Developer of the Original Code is Amino Communications Ltd.
Portions created by the Initial Developer are Copyright |copy| 2008
the Initial Developer. All Rights Reserved.

.. |copy| unicode:: 0xA9 .. copyright sign

Contributor(s):

  Amino Communications Ltd, Swavesey, Cambridge UK

.. ***** END LICENSE BLOCK *****
.. -------------------------------------------------------------------------------
.. vim: set filetype=rst expandtab shiftwidth=2:
