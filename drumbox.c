#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include <complex.h>
#include <fftw3.h>

#include <jack/jack.h>
#include <jack/midiport.h>

jack_port_t *input_port;
jack_port_t *output_port;
jack_client_t *client;

unsigned char note;

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define DEBUG_OUTPUT 1

#if DEBUG_OUTPUT
#define DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG(...)
#endif

void send_note(void *midi_out_buffer, jack_nframes_t ofs, float velocity) {
  jack_midi_data_t *midi_note =
    jack_midi_event_reserve(midi_out_buffer, 0, 3);

  midi_note[0] = 0x90;             /* noteon */
  midi_note[1] = note;             /* freq */
  midi_note[2] = MIN(127, (unsigned char) (velocity * 127)); /* velocity */

  DEBUG("sending note with velocity = %u\n", midi_note[2]);
}  

#define FFT_SIZE 128
fftwf_complex *fft_in, *fft_out;
fftwf_plan fft_plan;

typedef jack_default_audio_sample_t sample_t;
#define LAST_WIDTH FFT_SIZE
sample_t last[LAST_WIDTH];
unsigned int last_idx = 0;
unsigned int delay = 0;
sample_t power = 0;
bool hit = false;

unsigned int wrap(unsigned int i, unsigned int width) {
  while (i < 0) i += width;
  return i % width;
}

int process(jack_nframes_t nframes, void *arg) {
  sample_t *in = jack_port_get_buffer(input_port, nframes);

  void *midi_out = jack_port_get_buffer(output_port, nframes);
  jack_midi_clear_buffer(midi_out);

  for (unsigned int i = 0; i < nframes; i++) {
    sample_t cur = in[i];

    last_idx = wrap(last_idx + 1, LAST_WIDTH);
    last[last_idx] = fabs(cur);

    for (unsigned int j = 0; j < LAST_WIDTH; j++) {
      fft_in[j] = last[wrap(last_idx + j, LAST_WIDTH)];
    }

    fftwf_execute(fft_plan);

    power = 0;
    for (unsigned int j = 0; j < FFT_SIZE; j++) {
      power += fabs(fft_out[j]);
    }
    power /= FFT_SIZE;
    
    if (!hit && (power > 2)) {
      hit = true;
      DEBUG("HIT   power = %.3f \n", power);
      send_note(midi_out, i, 0.5);
    } else if (hit && (power < 0.9)) {
      hit = false;
      DEBUG("UNHIT power = %.3f \n", power);
    }
    
    static float minp;
    static float maxp;
    if (delay == 0) {
      DEBUG("power ∈ [%+2.3f, %+2.3f] \r", minp, maxp);
      /* unsigned int star = power; */
      /* for (unsigned int j = 0; j < star; j++) DEBUG(" "); */
      /* DEBUG("*\r"); */
      delay = 30000;
      minp = 1e9;
      maxp = -1e9;
    } else {
      if (power < minp) minp = power;
      if (power > maxp) maxp = power;
      
      --delay;
    }
  }
    
  return 0;      
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void jack_shutdown(void *arg) {
  exit(1);
}

int main(int argc, char **argv) {
  const char **ports;
  const char *client_name = "drumbox";
  const char *server_name = NULL;
  jack_options_t options = JackNullOption;
  jack_status_t status;

  /* Initialise FFTW */

  fft_in = fftwf_malloc(FFT_SIZE * sizeof *fft_in);
  fft_out = fftwf_malloc(FFT_SIZE * sizeof *fft_out);
  fft_plan = fftwf_plan_dft_1d(FFT_SIZE, fft_in, fft_out,
                               FFTW_FORWARD, FFTW_MEASURE);
  
  if (argc < 2) {
    note = 48;
  } else {
    note = atoi(argv[1]);
  }
  
  /* open a client connection to the JACK server */

  client = jack_client_open(client_name, options, &status, server_name);
  if (client == NULL) {
    DEBUG("jack_client_open() failed, "
            "status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      DEBUG("Unable to connect to JACK server\n");
    }
    exit(1);
  }
  if (status & JackServerStarted) {
    DEBUG("JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(client);
    DEBUG("unique name `%s' assigned\n", client_name);
  }

  /* tell the JACK server to call `process()' whenever
     there is work to be done.
  */

  jack_set_process_callback(client, process, 0);

  /* tell the JACK server to call `jack_shutdown()' if
     it ever shuts down, either entirely, or if it
     just decides to stop calling us.
  */

  jack_on_shutdown(client, jack_shutdown, 0);

  /* display the current sample rate. 
   */

  printf("engine sample rate: %" PRIu32 "\n",
         jack_get_sample_rate(client));

  /* create two ports */

  input_port = jack_port_register(client, "input",
                                  JACK_DEFAULT_AUDIO_TYPE,
                                  JackPortIsInput, 0);
  output_port = jack_port_register(client, "output",
                                   JACK_DEFAULT_MIDI_TYPE,
                                   JackPortIsOutput, 0);

  if ((input_port == NULL) || (output_port == NULL)) {
    DEBUG("no more JACK ports available\n");
    exit(1);
  }

  /* Tell the JACK server that we are ready to roll.  Our
   * process() callback will start running now. */

  if (jack_activate(client)) {
    DEBUG("cannot activate client");
    exit(1);
  }

  /* Connect the ports.  You can't do this before the client is
   * activated, because we can't make connections to clients
   * that aren't running.  Note the confusing (but necessary)
   * orientation of the driver backend ports: playback ports are
   * "input" to the backend, and capture ports are "output" from
   * it.
   */

  ports = jack_get_ports(client, NULL, NULL,
                         JackPortIsPhysical|JackPortIsOutput);
  if (ports == NULL) {
    DEBUG("no physical capture ports\n");
    exit(1);
  }

  if (jack_connect(client, ports[0], jack_port_name(input_port))) {
    DEBUG("cannot connect input ports\n");
  }

  free(ports);

  /* keep running until stopped by the user */

  sleep(-1);

  /* this is never reached but if the program
     had some other way to exit besides being killed,
     they would be important to call.
  */

  jack_client_close(client);
  exit(0);
}
