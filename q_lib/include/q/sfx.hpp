/*=============================================================================
   Copyright (c) 2014-2018 Joel de Guzman. All rights reserved.

   Distributed under the MIT License [ https://opensource.org/licenses/MIT ]
=============================================================================*/
#if !defined(CYCFI_Q_SFX_HPP_DECEMBER_24_2015)
#define CYCFI_Q_SFX_HPP_DECEMBER_24_2015

#include <cmath>
#include <algorithm>
#include <q/literals.hpp>
#include <q/support.hpp>
#include <q/fx.hpp>

namespace cycfi { namespace q
{
	using namespace literals;

   ////////////////////////////////////////////////////////////////////////////
   // Fast Downsampling with antialiasing. A quick and simple method of
   // downsampling a signal by a factor of two with a useful amount of
   // antialiasing. Each source sample is convolved with { 0.25, 0.5, 0.25 }
   // before downsampling. (from http://www.musicdsp.org/)
   //
   // This class is templated on the native integer sample type
   // (e.g. uint16_t).
   ////////////////////////////////////////////////////////////////////////////
   template <typename T>
   struct fast_downsample
   {
      T operator()(T s1, T s2)
      {
         auto out = x + (s1 >> 1);
         x = s2 >> 2;
         return out + x;
      }

      T x = 0.0f;
   };

   ////////////////////////////////////////////////////////////////////////////
   // dynamic_smoother based on Dynamic Smoothing Using Self Modulating Filter
   // by Andrew Simper, Cytomic, 2014, andy@cytomic.com
   //
   //    https://cytomic.com/files/dsp/DynamicSmoothing.pdf
   //
   // A robust and inexpensive dynamic smoothing algorithm based on using the
   // bandpass output of a 2 pole multimode filter to modulate its own cutoff
   // frequency. The bandpass signal is a meaure of how much the signal is
   // "changing" so is useful to increase the cutoff frequency dynamically
   // and allow for faster tracking when the input signal is changing more.
   // The absolute value of the bandpass signal is used since either a change
   // upwards or downwards should increase the cutoff.
   //
   ////////////////////////////////////////////////////////////////////////////
   struct dynamic_smoother
   {
      dynamic_smoother(frequency base, std::uint32_t sps)
       : dynamic_smoother(base, 0.5, sps)
      {}

      dynamic_smoother(frequency base, float sensitivity, std::uint32_t sps)
       : sense(sensitivity * 4.0f)  // efficient linear cutoff mapping
       , wc(double(base) / sps)
      {
         auto gc = std::tan(pi * wc);
         g0 = 2.0f * gc / (1.0f + gc);
      }

      float operator()(float s)
      {
         auto lowlz = low1;
         auto low2z = low2;
         auto bandz = lowlz - low2z;
         auto g = std::min(g0 + sense * std::abs(bandz), 1.0f);
         low1 = lowlz + g * (s - lowlz);
         low2 = low2z + g * (low1 - low2z);
         return low2z;
      }

      void base_frequency(frequency base, std::uint32_t sps)
      {
         wc = double(base) / sps;
         auto gc = std::tan(pi * wc);
         g0 = 2.0f * gc / (1.0f + gc);
      }

      float sense, wc, g0;
      float low1 = 0.0f;
      float low2 = 0.0f;
   };

   ////////////////////////////////////////////////////////////////////////////
   // Dynamic one pole low-pass filter (6dB/Oct). Essentially the same as
   // one_pole_lowpass but with the coefficient, a, supplied dynamically.
   //
   //    y: current value
   ////////////////////////////////////////////////////////////////////////////
   struct dynamic_lowpass
   {
      float operator()(float s, float a)
      {
         return y += a * (s - y);
      }

      float operator()() const
      {
         return y;
      }

      dynamic_lowpass& operator=(float y_)
      {
         y = y_;
         return *this;
      }

      float y = 0.0f;
   };

   ////////////////////////////////////////////////////////////////////////////
   // zero_cross generates pulses that coincide with the zero crossings
   // of the signal. To minimize noise, 1) we apply some amount of hysteresis
   // and 2) constrain the time between transitions to a minumum given
   // min_period (or max_freq).
   ////////////////////////////////////////////////////////////////////////////
   struct zero_cross
   {
      zero_cross(float hysteresis, frequency max_freq, std::uint32_t sps)
       : zero_cross(hysteresis, max_freq.period(), sps)
      {}

      zero_cross(float hysteresis, period min_period, std::uint32_t sps)
       : _hysteresis(hysteresis), _min_samples(double(min_period) * sps)
      {}

      float operator()(float s)
      {
         if (_count++ < _min_samples)
            return _state;

         if (s > _hysteresis && !_state)
         {
            _state = 1;
            _count = 0;
         }
         else if (s < -_hysteresis && _state)
         {
            _state = 0;
            _count = 0;
         }
         return _state;
      }

      bool edge() const { return _count == 0; }

      float const       _hysteresis = 0.0f;
      std::size_t const _min_samples;
      bool              _state = 0;
      std::size_t       _count = 0;
   };

   ////////////////////////////////////////////////////////////////////////////
   // peak generates pulses that coincide with the peaks of a waveform. This
   // is accomplished by comparing the signal with the (slightly attenuated)
   // envelope of the signal (env) using a schmitt_trigger.
   //
   //    droop: Envelope droop amount (attenuation)
   //    hysteresis: schmitt_trigger hysteresis amount
   //
   // The result is a bool corresponding to the peaks.
   ////////////////////////////////////////////////////////////////////////////
   struct peak
   {
      peak(float droop, float hysteresis)
       : _droop(droop), _cmp(hysteresis)
      {}

      bool operator()(float s, float env)
      {
         return _cmp(s, env * _droop);
      }

      float const       _droop;
      schmitt_trigger   _cmp;
   };

   ////////////////////////////////////////////////////////////////////////////
   struct onset
   {
      static constexpr auto droop = 0.8f;
      static constexpr auto hysteresis = 0.005f;

      onset(period min_period, std::uint32_t sps)
       : _min_samples(double(min_period) * sps)
      {}

      bool operator()(float s, float env)
      {
         if (_count++ < _min_samples)
            return _state;

         auto pk = _pk(s, env);
         if (!_state && pk)
         {
            if (_current_peak < s)
            {
               _current_peak = s;
               _state = 1;
               _count = 0;
            }
         }
         else if (_state && !pk)
         {
            _state = 0;
            _count = 0;
         }
         return _state;
      }

      float             peak_val() const { return _current_peak; }
      void              reset() { _current_peak = 0.0f; }

      peak              _pk { droop, hysteresis };
      std::size_t const _min_samples;
      bool              _state = 0;
      std::size_t       _count = 0;
      float             _current_peak = 0.0f;
   };
}}

#endif
