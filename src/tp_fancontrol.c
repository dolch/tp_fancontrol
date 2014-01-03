//-----------------------------------------------------------------------------
// Thinkpad Temperature Daemon
//
// Copyright (C) 2013 M.Girard
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <dirent.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <systemd/sd-daemon.h>

#define __DATALEN 32

#define __FAN "/proc/acpi/ibm/fan"

#define __CORETEMP "/sys/devices/platform/coretemp.0"

#define __CORETEMPIN "1"

//-----------------------------------------------------------------------------
// Name: State
//-----------------------------------------------------------------------------

enum state_e
  {    
    FAN_ANY = 9000,
    FAN_AUTO,
    FAN_FULLSPEED,
    FAN_MAXSPEED,
    
    EV_START,
    EV_TIMER,
    EV_STOP
  };

struct transition_s
{
  int st;
  int ev;
  int (*fn)(double, double, double);
};

static int _clear(double temp, double temp_max, double slope)
{
  return FAN_AUTO;
}

static int _auto(double temp, double temp_max, double slope)
{
  if (temp >= (temp_max - 10.0d))
    {
      return FAN_FULLSPEED;
    }

  return FAN_AUTO;
}

static int _fullspeed(double temp, double temp_max, double slope)
{
  if (temp >= temp_max)
    {
      return FAN_MAXSPEED;
    }

  if (temp < (temp_max - 10.0d))
    {
      if (slope > 200.0d)
	{
	  return FAN_AUTO;
	}
    }

  return FAN_FULLSPEED;
}

static int _maxspeed(double temp, double temp_max, double slope)
{
  if (temp < (temp_max - 10.0d))
    {
      if (slope > 200.0d)
	{
	  return FAN_AUTO;
	}
    }

  return FAN_MAXSPEED;
}

static const struct transition_s trans[] =
  {
    {FAN_ANY,       EV_START, &_clear},
    {FAN_AUTO,      EV_TIMER, &_auto},
    {FAN_FULLSPEED, EV_TIMER, &_fullspeed},
    {FAN_MAXSPEED,  EV_TIMER, &_maxspeed},
    {FAN_ANY,       EV_STOP,  &_clear}
  };

#define TRANS_COUNT (sizeof (trans) / sizeof (*trans))

//-----------------------------------------------------------------------------
// Name: Data
//-----------------------------------------------------------------------------

static const char *optstring = "h?t:";

static const struct option longopt[] =
  {
    { "help", no_argument, NULL, 'h' },
    { "temp",  optional_argument, NULL, 't' },
    { NULL, no_argument, NULL, 0 }
  };

struct sensor_s
{
  char *input;
  char *max;
  int temp[__DATALEN];
  int temp_max;
};

struct fan_s
{
  char *output;  
  int speed;
};

struct globalstate_t
{
  int interval;
  char *opt_fan;
  char *opt_coretemp;
  char opt_temp[16][4];
  int num_opt_temp;

  int interrupted;
  struct fan_s *fan;
  struct sensor_s *sensors;
  int num_sensors;
  
} gact;

//-----------------------------------------------------------------------------
// Name: Daemon
//-----------------------------------------------------------------------------

static int monitor_init(void);
static void monitor_deinit(void);
static void monitor_event(int);

//-----------------------------------------------------------------------------
// Name: System
//-----------------------------------------------------------------------------

int sys_initfan(const char *, struct fan_s *);
int sys_fan(struct fan_s *);
void sys_deinitfan(struct fan_s *);
int sys_initsensor(const char *, const char *, struct sensor_s *);
void sys_deinitsensor(struct sensor_s *);
int sys_sensor(struct sensor_s *);

//-----------------------------------------------------------------------------
// Name: main_signal
//-----------------------------------------------------------------------------
/*!
**
*/
void 
display_help()
{
  fprintf (stderr,
	   "tp_fan_control [OPTIONS...]"
	   "\n\n"
	   "Keep the fan running on auto unless we smell burning."
	   "\n\n"
	   "-h --help\t" "Show this help"
	   "\n"
	   "-t --temp\t" "Coretemp sensor identifier"
	   "\n\n"
	   "Fan:     \t" __FAN
	   "\n"
	   "Sensors: \t" __CORETEMP
	   "\n\n");
}

//-----------------------------------------------------------------------------
// Name: main_signal
//-----------------------------------------------------------------------------
/*!
**
*/
static void
main_signal(int signum)
{
  switch(signum)
    {
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGTERM:
    case SIGALRM:
      gact.interrupted = signum;

      break;
    }
}

//-----------------------------------------------------------------------------
// Name: main
//-----------------------------------------------------------------------------
/*!
**
*/
int
main(int argc, char *argv[])
{
  int opt, optindex;

  setlocale (LC_CTYPE, "");

  // state

  memset (&gact, 0, sizeof(gact));

  gact.interrupted = 0;

  gact.sensors = NULL;

  gact.num_sensors = 0;

  gact.fan = NULL;

  // signals

  struct sigaction signal_action;

  memset (&signal_action, 0, sizeof(signal_action));

  signal_action.sa_handler = &main_signal;

  sigaction (SIGINT, &signal_action, NULL);

  sigaction (SIGQUIT, &signal_action, NULL);

  sigaction (SIGTERM, &signal_action, NULL);

  sigaction (SIGHUP, &signal_action, NULL);

  sigaction (SIGALRM, &signal_action, NULL);

  // options - defaults

  gact.interval = 500;

  gact.opt_fan = __FAN;

  gact.opt_coretemp = __CORETEMP;

  gact.num_opt_temp = 0;

  // options

  opt = 0;

  optindex = 0;

  opt = getopt_long (argc, argv, optstring, longopt, &optindex);

  while (opt != -1)
    {
      switch (opt)
	{
	case 'h':
	case '?':
	  display_help ();

	  exit (EXIT_SUCCESS);

	  break;

	case 't':	  
	  if (gact.num_opt_temp < 16)
	    {
	      snprintf (&gact.opt_temp[gact.num_opt_temp][0], 4, "%s", optarg);

	      gact.num_opt_temp++;
	    }

	  break;

	default:
	  exit (EXIT_FAILURE);

	  break;
	}

      opt = getopt_long (argc, argv, optstring, longopt, &optindex);
    }

  if (gact.num_opt_temp == 0)
    {
      strcpy (&gact.opt_temp[0][0], __CORETEMPIN);

      gact.num_opt_temp++;
    }

  // ready

  sd_notify (0, "READY=1");

  fprintf (stderr, SD_INFO "startup **\n");

  if (monitor_init ())
    {
      fprintf (stderr, SD_ERR "shutdown ** unable to initialize monitor\n");

      exit (EXIT_FAILURE);
    }

  monitor_event (EV_START);

  // timer

  struct itimerval it_val;

  memset (&it_val, 0, sizeof(it_val));

  it_val.it_value.tv_sec = gact.interval / 1000;

  it_val.it_value.tv_usec = (gact.interval * 1000) % 1000000;

  it_val.it_interval = it_val.it_value;

  setitimer (ITIMER_REAL, &it_val, NULL);

  while (1)
    {
      sd_notify (0, "WATCHDOG=1");

      pause();

      if (gact.interrupted == SIGHUP)
	{
	  fprintf (stderr, SD_INFO "reloading **\n");
	}
      else if (gact.interrupted == SIGALRM)
	{
	  monitor_event (EV_TIMER);
	}
      else
	{
	  break;
	}
    }

  it_val.it_value.tv_sec = 0;

  it_val.it_value.tv_usec = 0;

  it_val.it_interval = it_val.it_value;

  setitimer (ITIMER_REAL, &it_val, NULL);

  // done

  monitor_event (EV_STOP);

  fprintf (stderr, SD_INFO "shutdown **\n");

  monitor_deinit ();

  exit (EXIT_SUCCESS);
}

//-----------------------------------------------------------------------------
// Name: monitor_init
//-----------------------------------------------------------------------------
/*!
**
*/
static int
monitor_init(void)
{
  struct fan_s out;

  struct sensor_s in;

  // fan

  if (sys_initfan (gact.opt_fan, &out) != 0)
    {
      return 1;
    }

  gact.fan = malloc (sizeof (out));

  memcpy (gact.fan, &out, sizeof (out));

  // sensors

  int i;
  
  for (i = 0; i < gact.num_opt_temp; ++i)
    {
      if (sys_initsensor (gact.opt_coretemp, &gact.opt_temp[i][0], &in) != 0)
	{
	  continue;
	}

      gact.num_sensors = gact.num_sensors + 1;
      
      gact.sensors = realloc (gact.sensors, gact.num_sensors * sizeof (in));
      
      memcpy (&gact.sensors[gact.num_sensors - 1], &in, sizeof (in));
    }
  
  if (gact.num_sensors == 0)
    {
      return 1;
    }

  return 0;
}

//-----------------------------------------------------------------------------
// Name: monitor_deinit
//-----------------------------------------------------------------------------
/*!
**
*/
static void
monitor_deinit(void)
{
  int i;

  for (i = 0; i < gact.num_sensors; i++)
    {
      sys_deinitsensor (&gact.sensors[i]);
    }

  free (gact.sensors);

  gact.sensors = NULL;

  gact.num_sensors = 0;

  sys_deinitfan (gact.fan);

  free (gact.fan);

  gact.fan = NULL;
}

//-----------------------------------------------------------------------------
// Name: monitor_event
//-----------------------------------------------------------------------------
/*!
**
*/
static void
monitor_event(int event)
{
  int t;

  int i, j, n;

  struct sensor_s *current;

  double max, m, b, sxx, sx, sy, sxy;

  n = 0;

  m = b = sxx = sx = sy = sxy = 0;

  max = 100000.0d;

  for (i = 0; i < gact.num_sensors; i++)
    {
      current = &gact.sensors[i];

      for (j = __DATALEN - 1; j > 0; --j)
	{
	  current->temp[j] = current->temp[j-1];
	}

      // temp

      current->temp[0] = sys_sensor (current);

      // temp max

      max = current->temp_max < max ? current->temp_max : max;

      // linear regression

      for (j = 0; j < __DATALEN; j++)
	{
	  if (current->temp[j] == 0)
	    {
	      break;
	    }
	  
	  sx += j;

	  sxx += j * j;

	  sy += current->temp[j];

	  sxy += j * current->temp[j];

	  n++;
	}
    }

  // y = mx + b

  m = (sxy - ((sx * sy) / n)) / (sxx - ((sx * sx) / n));

  b = (sy - (m * sx)) / n;
  
#if __DEBUG__

  fprintf (stderr, SD_DEBUG "%2d%15.5f%15.5f\n", j, b, m);
  
#endif
  
  // normal

  b /= 1000.0d;

  max /= 1000.0d;

  // transition
  
  for (t = 0; t < TRANS_COUNT; t++)
    {
      if ((gact.fan->speed == trans[t].st) || (FAN_ANY == trans[t].st))
	{
	  if (event == trans[t].ev)
	    {
	      // next
	      
	      gact.fan->speed = trans[t].fn (b, max, m);
	      
	      if (gact.fan->speed != trans[t].st)
		{
		  sys_fan (gact.fan);
		}

	      break;
	    }
	}
    }
}

//-----------------------------------------------------------------------------
// Name: sys_*fan
//-----------------------------------------------------------------------------
/*!
**
*/
int
sys_initfan(const char *fullname, struct fan_s *out)
{
  FILE *fp;

  size_t len;

  ssize_t read;

  char *line;

  int module_valid = 0;

  if ((fp = fopen (fullname, "r+")) == NULL)
    {
      return 1;
    }

  len = 0;

  line = NULL;

  while ((read = getline (&line, &len, fp)) != -1)
    {
      if (!strncmp ("commands:", line, 9))
	{
	  module_valid = 1;
	}
    }

  if (!module_valid)
    {
      return 1;
    }

  /*
    fprintf(fp, "watchdog %d\n", watchdog_timeout);
  */

  if (line)
    {
      free (line);

      line = NULL;
    }

  fclose (fp);

  out->output = strdup (fullname);

  out->speed = FAN_ANY;

  return 0;
}

void
sys_deinitfan(struct fan_s *out)
{
  free (out->output);

  out->output = NULL;

  out->speed = FAN_ANY;
}

int
sys_fan(struct fan_s *out)
{
  FILE *fp;

  const char *speed;

  switch (out->speed)
    {
    case FAN_AUTO:
      speed = "level auto";

      break;
    case FAN_MAXSPEED:
      speed = "level 7";

      break;
    case FAN_FULLSPEED:
      speed = "level full-speed";

      break;

    default:
      speed = NULL;

      break;
    }

  if (speed)
    {
      if ((fp = fopen (out->output, "r+")) == NULL)
	{
	  return 1;
	}
      else
	{
	  fprintf(fp, "%s", speed);

	  fclose (fp);

	  fprintf(stderr, "fan: %s\n", speed);
	}
    }

  return 0;
}

//-----------------------------------------------------------------------------
// Name: sys_*sensor
//-----------------------------------------------------------------------------
/*!
**
*/
int
sys_initsensor(const char *path, const char *name, struct sensor_s *in)
{
  int len, i;

  char buf[256];

  struct stat sb;

  FILE *fp;

  int temp_max;

  // input

  len = 0;

  len += snprintf (buf + len, sizeof(buf) - len, "%s", path);

  len += snprintf (buf + len, sizeof(buf) - len, "/temp%s_input", name);

  if (stat (buf, &sb) != 0)
    {
      return 1;
    }

  if (S_ISREG (sb.st_mode) == 0)
    {
      return 1;
    }

  in->input = strdup (buf);

  // max

  len = 0;

  len += snprintf (buf + len, sizeof(buf) - len, "%s", path);
  
  len += snprintf (buf + len, sizeof(buf) - len, "/temp%s_max", name);

  if ((fp = fopen (buf, "r")) != NULL)
    {
      if (fscanf (fp, "%d", &temp_max) != 1)
	{
	  temp_max = 0;
	}

      fclose (fp);
    }
  else
    {
      temp_max = 0;
    }

  if (temp_max == 0)
    {
      return 1;
    }
  
  in->max = strdup (buf);

  in->temp_max = temp_max;

  // data

  for (i = 0; i < __DATALEN; ++i)
    {
      in->temp[i] = 0;
    }

  return 0;
}

void
sys_deinitsensor(struct sensor_s *in)
{
  free (in->input);

  free (in->max);
}

int
sys_sensor(struct sensor_s *in)
{
  FILE *fp;

  int temp;

  if ((fp = fopen (in->input, "r")) != NULL)
    {
      if (fscanf (fp, "%d", &temp) != 1)
	{
	  temp = 0;
	}

      fclose (fp);
    }
  else
    {
      temp = 0;
    }

  return temp;
}
