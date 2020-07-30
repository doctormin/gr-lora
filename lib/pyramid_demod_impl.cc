/* -*- c++ -*- */
/* 
 * Copyright 2020 jkadbear.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "pyramid_demod_impl.h"

#define DEBUG_OFF                0
#define DEBUG_INFO               1
#define DEBUG_VERBOSE            2
#define DEBUG_VERBOSE_VERBOSE    3
#define DEBUG                    DEBUG_VERBOSE

#define DUMP_IQ       0

#define OVERLAP_FACTOR  16

namespace gr {
  namespace lora {

    pyramid_demod::sptr
    pyramid_demod::make( unsigned short spreading_factor,
                         bool  low_data_rate,
                         float beta,
                         unsigned short fft_factor,
                         float threshold,
                         float fs_bw_ratio)
    {
      return gnuradio::get_initial_sptr
        (new pyramid_demod_impl(spreading_factor, low_data_rate, beta, fft_factor, threshold, fs_bw_ratio));
    }

    /*
     * The private constructor
     */
    pyramid_demod_impl::pyramid_demod_impl( unsigned short spreading_factor,
                            bool  low_data_rate,
                            float beta,
                            unsigned short fft_factor,
                            float threshold,
                            float fs_bw_ratio)
      : gr::block("pyramid_demod",
              gr::io_signature::make(1, 1, sizeof(gr_complex)),
              gr::io_signature::make(0, 0, 0)),
        f_raw("raw.out", std::ios::out),
        f_fft("fft.out", std::ios::out),
        f_up_windowless("up_windowless.out", std::ios::out),
        f_up("up.out", std::ios::out),
        f_down("down.out", std::ios::out),
        d_sf(spreading_factor),
        d_ldr(low_data_rate),
        d_beta(beta),
        d_fft_size_factor(fft_factor),
        d_threshold(threshold)
    {
      assert((d_sf > 5) && (d_sf < 13));
      if (d_sf == 6) assert(!header);
      assert(d_fft_size_factor > 0);
      assert(((int)fs_bw_ratio) == fs_bw_ratio);
      d_p = (int) fs_bw_ratio;

      d_out_port = pmt::mp("out");
      message_port_register_out(d_out_port);

      d_state = PS_RESET;

      d_num_symbols = (1 << d_sf);
      d_num_samples = d_p*d_num_symbols;
      d_bin_size = d_fft_size_factor*d_num_symbols;
      d_fft_size = d_fft_size_factor*d_num_samples;
      d_fft = new fft::fft_complex(d_fft_size, true, 1);
      d_overlaps = OVERLAP_FACTOR;
      d_ttl = 6*d_overlaps; // MAGIC
      d_offset = 0;

      d_window = fft::window::build(fft::window::WIN_KAISER, d_num_samples, d_beta);

      d_ts_ref  = 0;
      d_bin_ref = 0;
      d_bin_tolerance = (d_ldr ? d_fft_size_factor * 2 : d_fft_size_factor / 2);

      d_power     = .000000001;     // MAGIC
      // d_threshold = 0.005;          // MAGIC
      // d_threshold = 0.003;          // MAGIC
      // d_threshold = 0.12;           // MAGIC

      // Create local chirp tables.  Each table is 2 chirps long to allow memcpying from arbitrary offsets.
      for (int i = 0; i < d_num_samples; i++) {
        double phase = M_PI/d_p*(i-i*i/(float)d_num_samples);
        d_downchirp.push_back(gr_complex(std::polar(1.0, phase)));
        d_upchirp.push_back(gr_complex(std::polar(1.0, -phase)));
      }

      unsigned short track_size = 40; // MAGIC
      d_num_preamble = 6; // MAGIC
      d_track.resize(track_size);
      for (auto & v : d_track)
      {
        v.reserve(d_overlaps * (d_num_preamble + 2));
      }
      d_bin_track_id_list.reserve(track_size);
      for (unsigned short i = 0; i < track_size; i++)
      {
        d_track_id_pool.push_back(i);
      }

      unsigned short packet_id_size = 40; // MAGIC
      d_packet.resize(packet_id_size);
      d_packet_state_list.reserve(packet_id_size);
      for (unsigned short i = 0; i < packet_id_size; i++)
      {
        d_packet_id_pool.push_back(i);
      }

      set_history(DEMOD_HISTORY_DEPTH*d_num_samples);  // Sync is 2.25 chirp periods long
    }

    /*
     * Our virtual destructor.
     */
    pyramid_demod_impl::~pyramid_demod_impl()
    {
      delete d_fft;
    }

    unsigned int
    pyramid_demod_impl::argmax_32f(float *fft_result, 
                       bool update_squelch, float *max_val_p)
    {
      float mag   = abs(fft_result[0]);
      float max_val = mag;
      unsigned int   max_idx = 0;

      for (unsigned int i = 0; i < d_bin_size; i++)
      {
        mag = abs(fft_result[i]);
        if (mag > max_val)
        {
          max_idx = i;
          max_val = mag;
        }
      }

      if (update_squelch)
      {
        d_power = max_val;
        d_squelched = (max_val > d_threshold) ? false : true;
      }

      *max_val_p = max_val;
      return max_idx;
    }

    unsigned short
    pyramid_demod_impl::argmax(gr_complex *fft_result, 
                       bool update_squelch)
    {
      float magsq   = pow(real(fft_result[0]), 2) + pow(imag(fft_result[0]), 2);
      float max_val = magsq;
      unsigned short   max_idx = 0;


      for (unsigned short i = 0; i < d_fft_size; i++)
      {
        magsq = pow(real(fft_result[i]), 2) + pow(imag(fft_result[i]), 2);
        if (magsq > max_val)
        {
          max_idx = i;
          max_val = magsq;
        }
      }

      if (update_squelch)
      {
        d_power = max_val;
        d_squelched = (d_power > d_threshold) ? false : true;
      }

      return max_idx;
    }

    void
    pyramid_demod_impl::find_and_add_peak(float *fft_mag, float *fft_mag_w)
    {
      for (unsigned int i = 0; i < d_bin_size; i++)
      {
        // find peak: search a local max with value larger than d_threshold
        unsigned int l_idx = pos_mod(i-1, d_bin_size);
        unsigned int r_idx = pos_mod(i+1, d_bin_size);
        if(fft_mag_w[i] > d_threshold && fft_mag_w[i] > fft_mag_w[l_idx] && fft_mag_w[i] > fft_mag_w[r_idx])
        {
          // this is a peak, insert it into peak track
          unsigned int cur_bin = pos_mod(d_bin_size + i - d_bin_ref, d_bin_size);
          bool found = false;
          unsigned short track_id;
          for (auto & bt: d_bin_track_id_list)
          {
            unsigned int dis = pos_mod(d_bin_size + cur_bin - bt.bin, d_bin_size);
            #if DEBUG >= DEBUG_VERBOSE_VERBOSE
              std::cout << "dis: " << dis << ", bt.bin: " << bt.bin << std::endl;
            #endif
            // Abs(current_bin - target_bin) < d_bin_tolerance
            if (dis <= d_bin_tolerance || dis >= d_bin_size - d_bin_tolerance)
            {
              found = true;
              track_id = bt.track_id;
              bt.updated = true;
              break;
            }
          }
          if (!found)
          {
            if (d_track_id_pool.empty())
            {
              std::cerr << "Current threshold is low! Increase the threshold or track size" << std::endl;
              exit(-1);
            }
            track_id = d_track_id_pool.front();
            d_track_id_pool.pop_front();
            d_bin_track_id_list.push_back(bin_track_id(cur_bin, track_id, true));
          }

          #if DEBUG >= DEBUG_VERBOSE_VERBOSE
            std::cout << "bin: " << i << ", ref bin: " << d_bin_ref << ", peak height: " << fft_mag[l_idx] << " "  << fft_mag[i] << " " << fft_mag[r_idx] << std::endl;
            std::cout << "track id: " << track_id << ", track size: " << d_track[track_id].size() << ", track id pool size: " << d_track_id_pool.size() << std::endl;
          #endif
          d_track[track_id].push_back(peak(d_ts_ref, i, fft_mag[i]));
        }
      }
    }

    symbol_type pyramid_demod_impl::get_central_peak(unsigned short track_id, peak & pk)
    {
      auto track = d_track[track_id];
      unsigned short len = track.size();

      #if DEBUG >= DEBUG_VERBOSE
        std::cout << "track id: " << track_id << ", track height: ";
        for (auto & pk : track)
        {
          std::cout << pk.h << ", ";
        }
        std::cout << std::endl;
      #endif
      if (len >= d_overlaps*(d_num_preamble-1) + 2)
      {
        // preamble
        unsigned short l_idx = len/2 - d_overlaps*(d_num_preamble-1)/2;
        unsigned short r_idx = (len-1)/2 + d_overlaps*(d_num_preamble-1)/2;
        if (track[l_idx].h > track[r_idx].h)
        {
          pk.ts  = track[l_idx].ts + d_num_samples/4 + (d_num_preamble-1)*d_num_samples;
          pk.bin = track[l_idx].bin;
        }
        else 
        {
          pk.ts  = track[r_idx].ts + d_num_samples/4;
          pk.bin = track[r_idx].bin;
        }
        float sum = 0;
        for (unsigned int i = d_overlaps*2; i < d_overlaps*(d_num_preamble-2); i++)
        {
          sum += track[i].h;
        }
        pk.h = sum / (d_overlaps*(d_num_preamble-4));
        return SYMBOL_PREAMBLE;
      }
      // TODO filter noise peak
      else if (len >= 2 && len <= 2*d_overlaps)
      {
        // get apex of this peak tracking
#if APEX_ALGORITHM == APEX_ALGORITHM_SEGMENT
        float max_h = track[0].h;
        unsigned short idx = 0;
        for (unsigned short i = 1; i < len; i++)
        {
          if (track[i].h > max_h)
          {
            max_h = track[i].h;
            idx = i;
          }
        }
        pk.ts  = track[idx].ts;
        pk.bin = track[idx].bin;
        pk.h   = track[idx].h;
#elif APEX_ALGORITHM == APEX_ALGORITHM_LINEAR_REGRESSION
#endif
        return SYMBOL_DATA;
      }
      // TODO special case: more than two consecutive same data symbols

      return SYMBOL_BROKEN_DATA;
    }

    bool
    pyramid_demod_impl::add_symbol_to_packet(peak & pk, symbol_type st)
    {
      #if DEBUG >= DEBUG_VERBOSE
        std::cout << "symbol type: ";
        if (st == SYMBOL_PREAMBLE) std::cout << "PREAMBLE" << std::endl;
        else if (st == SYMBOL_DATA) std::cout << "DATA" << std::endl;
        else std::cout << "BROKEN" << std::endl;
      #endif

      if (st == SYMBOL_PREAMBLE)
      {
        // preamble detected, create a new packet
        unsigned short pkt_id = d_packet_id_pool.front();
        d_packet_id_pool.pop_front();
        d_packet[pkt_id].push_back(pk);
        d_packet_state_list.push_back(packet_state(pkt_id, d_ttl));
        #if DEBUG >= DEBUG_INFO
          std::cout << "New preamble detected (ts:" << std::fixed << std::setprecision(2) << pk.ts/(float)d_num_samples << ", bin:" << pk.bin << ", h:" << pk.h << ") Packet#" << pkt_id << std::endl;
        #endif
        return true;
      }
      else if (st == SYMBOL_DATA)
      {
        unsigned short pkt_idx  = 0;
        unsigned short pkt_id   = 0;
        float min_dis = std::numeric_limits<float>::infinity();
        bool found = false;

        for (unsigned int i = 0; i < d_packet_state_list.size(); i++)
        {
          // put peak into the best matched packet
          auto const & ps = d_packet_state_list[i];
          unsigned int ts_dis = pos_mod(pk.ts - d_packet[ps.packet_id][0].ts, TIMESTAMP_MOD);
          // candidate symbols must have valid timestamp
          if (ts_dis > 4 * d_num_samples && ts_dis < TIMESTAMP_MOD / 2)
          {
            float dis = pos_mod(ts_dis, d_num_samples) / (float) d_num_samples;
            // In definition above, "dis->0" and "dis->1" both mean their timestamp differences to the actual timestamp is small
            // therefore we transform dis to let "dis->1" represent larger difference
            dis = (dis > 0.5) ? (1 - dis) * 2 : dis * 2;
            // dis += std::abs(d_packet[ps.packet_id][0].h - pk.h) / d_packet[ps.packet_id][0].h;
            if (dis < min_dis)
            {
              found     = true;
              pkt_idx   = i;
              pkt_id    = ps.packet_id;
              min_dis   = dis;
            }
          }
        }

        if (found)
        {
          // new symbol added, reset TTL of the packet
          d_packet_state_list[pkt_idx].ttl = d_ttl;
          d_packet[pkt_id].push_back(pk);
          #if DEBUG >= DEBUG_INFO
            std::cout << "Add symbol (ts:" << std::fixed << std::setprecision(2) << pk.ts/(float)d_num_samples << ", bin:" << pk.bin << ", h:" << pk.h << ") to Packet#" << pkt_id << std::endl;
          #endif
          return true;
        }
        else
        {
          #if DEBUG >= DEBUG_INFO
            std::cout << "packet_state_list size: " << d_packet_state_list.size() << ", failed to classify symbol (ts:" << std::fixed << std::setprecision(2) << pk.ts/(float)d_num_samples << ", bin:" << pk.bin << ", h:" << pk.h << ")" << std::endl;
          #endif
          return false;
        }
      }
      else
      {
        #if DEBUG >= DEBUG_INFO
          std::cout << "Unrecognized symbol type!" << std::endl;
        #endif
        return false;
      }
    }

    void
    pyramid_demod_impl::check_and_update_track()
    {
      int erase_cnt = 0;
      for (auto const & bt : d_bin_track_id_list)
      {
        if (!bt.updated)
        {
          erase_cnt++ ;
          // this peak tracking is over, extract the apex
          peak pk(0, 0, 0);
          symbol_type st = get_central_peak(bt.track_id, pk);
          // #if DEBUG >= DEBUG_INFO
          //   if (st == SYMBOL_PREAMBLE || st == SYMBOL_DATA)
          //   {
          //     std::cout << "Add new symbol (ts:" << pk.ts << ", bin:" << pk.bin << ", h:" << pk.h << ")" << std::endl;
          //   }
          // #endif
          if (st == SYMBOL_PREAMBLE || st == SYMBOL_DATA)
          {
            bool res = add_symbol_to_packet(pk, st);
            #if DEBUG >= DEBUG_VERBOSE
              if (!res)
              {
                std::cout << "Failed to add symbol to packet." << std::endl;
              }
            #endif
          }
          d_track_id_pool.push_back(bt.track_id); // id recycle
          d_track[bt.track_id].clear();           // track vector recycle
        }
      }
      // #if DEBUG >= DEBUG_VERBOSE
      //   std::cout << "size before erase: " << d_bin_track_id_list.size() << ", erase_cnt: " << erase_cnt << std::endl;
      // #endif
      d_bin_track_id_list.erase(
        std::remove_if(
          d_bin_track_id_list.begin(),
          d_bin_track_id_list.end(),
          [](bin_track_id & bt) { return !bt.updated; }
        ),
        d_bin_track_id_list.end()
      );
      // #if DEBUG >= DEBUG_VERBOSE
      //   std::cout << "size after erase: " << d_bin_track_id_list.size() << std::endl;
      // #endif
      for (auto & bt : d_bin_track_id_list)
      {
        bt.updated = false;
      }
    }

    void
    pyramid_demod_impl::forecast (int noutput_items,
                          gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = noutput_items * (1 << d_sf);
    }

    int
    pyramid_demod_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      if (ninput_items[0] < 4*d_num_samples) return 0;
      const gr_complex *in        = (const gr_complex *)  input_items[0];
      unsigned int  *out          = (unsigned int   *) output_items[0];
      unsigned int num_consumed   = d_num_samples / d_overlaps;
      float max_val               = 0;
      unsigned int max_index_sfd  = 0;
      float max_val_sfd           = 0;
      unsigned int tmp_idx        = 0;
      // #if DEBUG >= DEBUG_VERBOSE
      //   std::cout << "d_num_samples: " << d_num_samples <<  ", d_overlaps: " << d_overlaps << ", num_consumed: " << num_consumed << ", ts: " << d_ts_ref << std::endl;
      // #endif

      // Nomenclature:
      //  up_block   == de-chirping buffer to contain upchirp features: the preamble, sync word, and data chirps
      //  down_block == de-chirping buffer to contain downchirp features: the SFD
      gr_complex *buffer     = (gr_complex *)volk_malloc(d_fft_size*sizeof(gr_complex), volk_get_alignment());
      gr_complex *up_block   = (gr_complex *)volk_malloc(d_fft_size*sizeof(gr_complex), volk_get_alignment());
      gr_complex *up_block_w = (gr_complex *)volk_malloc(d_fft_size*sizeof(gr_complex), volk_get_alignment());
      gr_complex *down_block = (gr_complex *)volk_malloc(d_fft_size*sizeof(gr_complex), volk_get_alignment());
      float *fft_res_mag     = (float*)volk_malloc(d_fft_size*sizeof(float), volk_get_alignment());
      float *fft_res_add     = (float*)volk_malloc(d_bin_size*sizeof(float), volk_get_alignment());
      float *fft_res_add_w   = (float*)volk_malloc(d_fft_size*sizeof(float), volk_get_alignment());

      if (buffer == NULL || up_block == NULL || down_block == NULL)
      {
        std::cerr << "Unable to allocate processing buffer!" << std::endl;
      }

      // Dechirp the incoming signal
      volk_32fc_x2_multiply_32fc(down_block, in, &d_upchirp[0], d_num_samples);

      if (d_state == PS_READ_HEADER || d_state == PS_READ_PAYLOAD)
      {
        volk_32fc_x2_multiply_32fc(up_block, in, &d_downchirp[0], d_num_samples);
      }
      else
      {
        volk_32fc_x2_multiply_32fc(up_block, in, &d_downchirp[0], d_num_samples);
      }

      // Enable to write IQ to disk for debugging
      #if DUMP_IQ
        f_up_windowless.write((const char*)&up_block[0], d_num_samples*sizeof(gr_complex));
      #endif

      // Windowing
      volk_32fc_32f_multiply_32fc(up_block_w, up_block, &d_window[0], d_num_samples);

      #if DUMP_IQ
        if (d_state != PS_SFD_SYNC) f_down.write((const char*)&down_block[0], d_num_samples*sizeof(gr_complex));
        f_up.write((const char*)&up_block[0], d_num_samples*sizeof(gr_complex));
      #endif

      // Preamble and Data FFT
      // If d_fft_size_factor is greater than 1, the rest of the sample buffer will be zeroed out and blend into the window
      memset(d_fft->get_inbuf(),            0, d_fft_size*sizeof(gr_complex));
      memcpy(d_fft->get_inbuf(), &up_block[0], d_num_samples*sizeof(gr_complex));
      d_fft->execute();
      #if DUMP_IQ
        f_fft.write((const char*)d_fft->get_outbuf(), d_fft_size*sizeof(gr_complex));
      #endif

      // fft result magnitude summation
      volk_32fc_magnitude_32f(fft_res_mag, d_fft->get_outbuf(), d_fft_size);
      volk_32f_x2_add_32f(fft_res_add, fft_res_mag, &fft_res_mag[d_bin_size], d_bin_size);
      volk_32f_x2_add_32f(fft_res_add, fft_res_add, &fft_res_mag[d_fft_size-2*d_bin_size], d_bin_size);
      volk_32f_x2_add_32f(fft_res_add, fft_res_add, &fft_res_mag[d_fft_size-d_bin_size], d_bin_size);

      // apply FFT on windowed signal
      memset(d_fft->get_inbuf(),              0, d_fft_size*sizeof(gr_complex));
      memcpy(d_fft->get_inbuf(), &up_block_w[0], d_num_samples*sizeof(gr_complex));
      d_fft->execute();
      volk_32fc_magnitude_32f(fft_res_mag, d_fft->get_outbuf(), d_fft_size);
      volk_32f_x2_add_32f(fft_res_add_w, fft_res_mag, &fft_res_mag[d_bin_size], d_bin_size);
      volk_32f_x2_add_32f(fft_res_add_w, fft_res_add_w, &fft_res_mag[d_fft_size-2*d_bin_size], d_bin_size);
      volk_32f_x2_add_32f(fft_res_add_w, fft_res_add_w, &fft_res_mag[d_fft_size-d_bin_size], d_bin_size);

      // 1. peak tracking
      find_and_add_peak(fft_res_add, fft_res_add_w);
      // 2. kick out those peaks without update, push them into packet list
      check_and_update_track();
      // 3. check packets
      for (auto const & ps : d_packet_state_list)
      {
        #if DEBUG >= DEBUG_VERBOSE_VERBOSE
          std::cout << "packet id: " << ps.packet_id << ", ttl: " << ps.ttl << std::endl;
        #endif
        if (ps.ttl <= 0)
        {
          // send the demodulation result to decoder
          std::vector<unsigned short> symbols;

          auto & pkt = d_packet[ps.packet_id];
          unsigned int pre_ts  = pkt[0].ts;     // preamble timestamp
          unsigned int pre_bin = pkt[0].bin;    // preamble bin
          float        pre_h   = pkt[0].h;      // preamble peak height
          #if DEBUG >= DEBUG_VERBOSE_VERBOSE
            std::cout << "preamble ts: " << pre_ts << ", preamble bin: " << pre_bin << std::endl;
            std::cout << "ts: ";
            for (unsigned int i = 1; i < pkt.size(); i++)
            {
              std::cout << pkt[i].ts << ", ";
            }
            std::cout << std::endl << "bin: ";
            for (unsigned int i = 1; i < pkt.size(); i++)
            {
              std::cout << pkt[i].bin << ", ";
            }
            std::cout << std::endl;

            std::cout << " d_packet size: ";
            for (unsigned int i = 0; i < d_packet.size(); i++)
            {
              std::cout << d_packet[i].size() << ",";
            }
            std::cout << std::endl;

            std::cout << "current packet id: " << ps.packet_id << ", d_packet id: ";
            for (unsigned int i = 0; i < d_packet_id_pool.size(); i++)
            {
              std::cout << d_packet_id_pool[i] << ",";
            }
            std::cout << std::endl;
          #endif

          #if DEBUG >= DEBUG_INFO
            std::cout << "Finished packet: ";
          #endif

          // set relative timestamp to preamble
          // then ts_preamble = 0
          for (auto & pk : pkt)
          {
            pk.ts = pos_mod(pk.ts-pre_ts, TIMESTAMP_MOD);
          }
          pre_ts = 0;

          // sort peaks by their timestamps
          sort(pkt.begin(), pkt.end(),
            [](const peak & a, const peak & b) -> bool
          {
            return a.ts < b.ts;
          });

          #if DEBUG >= DEBUG_VERBOSE
            std::cout << std::endl;
            for (auto & pk : pkt)
            {
              std::cout << "(ts: " << pk.ts/(float)d_num_samples << ", bin: " << pk.bin << ", h: " << pk.h << ")" << std::endl;
            }
          #endif

          // LoRa PHY: Preamble + NetID(2) + SFD(2.25) + Data Payload
          // There are 4.25 symbols between preamble and data payload
          // ts_data - ts_preamble = 5*d_num_samples (ts_preamble has 0.25 fix in our implementation)
          // The first data symbol timestamp is in ts_preamble+[4.5,5.5]*d_num_samples
          unsigned int ts_interval_l = 4*d_num_samples + d_num_samples/2;
          // "i" starts from 1, ignoring the first preamble symbol
          for (unsigned int start_idx = 1; start_idx < pkt.size(); )
          {
            bool is_first = true;
            bool found = false;
            unsigned int end_idx = start_idx;
            for (; end_idx < pkt.size(); end_idx++)
            {
              if (is_first)
              {
                if ( pkt[end_idx].ts > ts_interval_l && pkt[end_idx].ts < ts_interval_l + d_num_samples )
                {
                  start_idx = end_idx;
                  is_first = false;
                  found = true;
                  #if DEBUG >= DEBUG_VERBOSE
                    std::cout << std::endl << "pkt[end_idx].ts: " << pkt[end_idx].ts/(float)d_num_samples << ", ts_interval: " << ts_interval_l/(float)d_num_samples << ", r: " << (ts_interval_l + d_num_samples)/(float)d_num_samples << std::endl;
                  #endif
                }
              }
              else
              {
                if (!( pkt[end_idx].ts > ts_interval_l && pkt[end_idx].ts < ts_interval_l + d_num_samples ))
                {
                  break;
                }
              }
            }

            if (found)
            {
              // search the best matched peak for the packet
              float min_dis = std::numeric_limits<float>::infinity();
              unsigned int idx = start_idx;
              for (unsigned int i = start_idx; i < end_idx; i++)
              {
                float dis = pos_mod(pkt[i].ts - pre_ts, d_num_samples) / (float) d_num_samples;
                // In definition above, "dis->0" and "dis->1" both mean their timestamp differences to the actual timestamp is small
                // therefore we transform dis to let "dis->1" represent larger difference
                dis = (dis > 0.5) ? (1 - dis) * 2 : dis * 2;
                dis += std::abs(pkt[i].h - pre_h) / pre_h;
                if (dis < min_dis)
                {
                  min_dis = dis;
                  idx = i;
                }
              }

              // ts could have been overflowed
              int bin_shift = pos_mod(pkt[idx].ts - pre_ts, d_num_samples) * d_bin_size / d_num_samples;
              unsigned int bin = pos_mod(pkt[idx].bin - pre_bin - bin_shift, d_bin_size);
              symbols.push_back(bin / d_fft_size_factor);

              #if DEBUG >= DEBUG_VERBOSE
                std::cout << "bin: " << bin / d_fft_size_factor << ", packet bin: " << pkt[idx].bin << ", bin_shift: " << bin_shift << std::endl;
              #endif
              #if DEBUG >= DEBUG_INFO
                std::cout << bin / d_fft_size_factor << ",";
              #endif
            }
            else
            {
              symbols.push_back(0);
              #if DEBUG >= DEBUG_INFO
                std::cout << "missing,";
              #endif
            }

            start_idx = end_idx;
            ts_interval_l = pos_mod(ts_interval_l + d_num_samples, TIMESTAMP_MOD);
          }
          #if DEBUG >= DEBUG_INFO
            std::cout << std::endl;
          #endif

          // LoRa data payload has at least 8 symbols
          if (symbols.size() >= 8)
          {
            pmt::pmt_t output = pmt::init_u16vector(symbols.size(), symbols);
            pmt::pmt_t msg_pair = pmt::cons(pmt::make_dict(), output);
            message_port_pub(d_out_port, msg_pair);
          }

          pkt.clear();
          d_packet_id_pool.push_back(ps.packet_id);
        }
      }

      // packets with negative ttl have been sent to decoder by the above for loop
      d_packet_state_list.erase(
        std::remove_if(
          d_packet_state_list.begin(),
          d_packet_state_list.end(),
          [](packet_state const & ps) { return ps.ttl <= 0; }
        ),
        d_packet_state_list.end()
      );
      for (auto & ps : d_packet_state_list)
      {
        ps.ttl -= 1;
      }

      d_ts_ref   = pos_mod(d_ts_ref + d_num_samples / d_overlaps, TIMESTAMP_MOD);
      d_bin_ref  = pos_mod(d_bin_ref + d_bin_size / d_overlaps, d_bin_size);

      #if DUMP_IQ
        f_raw.write((const char*)&in[0], num_consumed*sizeof(gr_complex));
      #endif

      consume_each (num_consumed);

      free(down_block);
      free(up_block);
      free(up_block_w);
      free(buffer);
      free(fft_res_mag);
      free(fft_res_add);
      free(fft_res_add_w);

      return noutput_items;
    }

  } /* namespace lora */
} /* namespace gr */
