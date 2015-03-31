/**********************************************************************
 * nistkasten.c
 * 
 * Handles all low level io with the PiFace
 * 
 * Jan Arnhold, April 2013
 * 
 **********************************************************************/

#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sqlite3.h>

extern "C" {
#include <libpiface-1.0/pfio.h>
}

// nanoseconds per millisecond
const long MSEC = 1000000;
// timeslices
const long TIMESLICE_NORM = 200;   // milliseconds
const long TIMESLICE_FAST = 100;  // milliseconds

// output sensors
const char OPIN_IR_SENSOR = 7;
const char OPIN_LIGHT = 8;

// input sensors
const char IPIN_QUIT =4;
const char IPIN_INNER_SENSOR = 1;
const char IPIN_OUTER_SENSOR = 2;

// Type of entry in database
const int SENSOR_EVTYPE_INOUT = 0;

// Logbook entries
const int LOG_FAIL_SENSOR = 100;
const int LOG_INFO_SENSOR = 10000;

// Time Taking photos
const int PHOTO_AFTER_EVENT_CNT = 1;
const int PHOTO_AFTER_EVENT_INTERVAL = 10000;


#define PHOTO_CMD "/home/jan/prg/ruby/vogelhaus/lib/photo_p.sh"
#define PHOTO_PAR_ENTERED "entered"
#define PHOTO_PAR_SENSOR "sensor"

#define PIPE_FILENAME "/tmp/birdbox.1"


/**********************************************************************
 * Time measuring functions
 * 
 * 
 *********************************************************************/
const unsigned long MAX_DURATION = 0xfffffff0;

const unsigned char DURATION_INNER_SENSOR = 0;
const unsigned char DURATION_WAIT_NEXT_PHOTO = 1;
const unsigned char DURATION_MAX = 2;
unsigned long g_duration_msec[DURATION_MAX];

/* 
 * Initialize all duration counters
 */
void init_duration()
{
	for( unsigned char i = 0; i < DURATION_MAX; i++ ){
		g_duration_msec[i] = 0;
	}
}

/*
 * Reset a given duration counter
 */
inline void reset_duration( unsigned char counter )
{
	g_duration_msec[counter] = 0;
}

/*
 * Increase duration counter, as long as it is smaller than the 
 * maximum value.
 */
inline void inc_duration( unsigned char counter, unsigned long step )
{
	if( g_duration_msec[counter] < MAX_DURATION ){
		g_duration_msec[counter] += step; 
	}
}

/*
 * returns a given duration counter in milli seconds
 */
inline unsigned long get_duration( unsigned char counter )
{
	return g_duration_msec[counter];
}

/*
 * checks whether a counter has reached it's waiting time
 */
inline char is_timeout( unsigned char counter, unsigned long waiting_time )
{
	return get_duration( counter ) >= waiting_time ? 1 : 0;
}


void ms_sleep( int msec )
{
  static struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = msec * MSEC;
  nanosleep( &ts, NULL );
}


/**********************************************************************
 * Input pin states
 * 
 * 
 *********************************************************************/

inline int is_active( char input_state, char input_bin_bit_mask )
{
	return ( input_state & input_bin_bit_mask ) == 0;
}

inline int not_active( char input_state, char input_bin_bit_mask )
{
	return ( input_state & input_bin_bit_mask ) > 0;
}


/**********************************************************************
 * Output pin states
 * 
 * 
 *********************************************************************/

void sensor( char enabled = 1 )
{
	pfio_digital_write( OPIN_IR_SENSOR, enabled );
}

void led_light( char enabled = 1 )
{
	pfio_digital_write( OPIN_LIGHT, enabled );
}

/**********************************************************************
 * 
 * Named Pipe functions
 *
 **********************************************************************/
int fd_birdbox_cli;
const int PIPE_BUF_LEN = 100;
char pipe_buf[PIPE_BUF_LEN];

void pipe_init()
{
	unlink( PIPE_FILENAME );

	if((mkfifo( PIPE_FILENAME, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == -1)
    {
      fprintf(stderr, "Could not create named pipe\n");
      exit(0);
    }
	
	/*We open the fifo for read and write*/
  	if((fd_birdbox_cli = open( PIPE_FILENAME, O_RDWR)) == - 1)
    {
      fprintf(stderr, "Can't open the fifo.....\n");
      exit(0);
    }
}

void pipe_close()
{
	close( fd_birdbox_cli );
}

void pipe_send_event( int event )
{
	snprintf( pipe_buf, PIPE_BUF_LEN, "%d\n", event);
	write(fd_birdbox_cli, pipe_buf, strlen(pipe_buf)) ;
}


/**********************************************************************
 * 
 * Database functions
 * 
 **********************************************************************/
const char* DATABASE_FILE = "/home/jan/prg/ruby/vogelhaus/db/development.sqlite3";
char db_cmd[256];

/*
 * 
 * name: add_to_db
 * @param type (0 ... undefined, 1 ... comming home, 2 ... leaving home)
 * @return
 * 
 * Adds a new 
 */
void add_to_db( int type )
{
	sqlite3 *db_handler;
	
	snprintf( db_cmd, 256, "INSERT INTO tuersensors (movetype, time, created_at, updated_at) VALUES ( %d, datetime('now','localtime'), datetime('now','localtime'), datetime('now','localtime'));",
		type );
		
	sqlite3_open(DATABASE_FILE, &db_handler);
	sqlite3_exec(db_handler, db_cmd, NULL, NULL, NULL );
	sqlite3_close(db_handler);
}
 
void logtext( int eventtype, const char* ptr_msg )
{
	sqlite3 *db_handler;
	
	snprintf( db_cmd, 256, "INSERT INTO logtexts (eventtype, msg, created_at, updated_at) VALUES ( %d, '%s', datetime('now','localtime'), datetime('now','localtime'));",
		eventtype, ptr_msg );
		
	sqlite3_open(DATABASE_FILE, &db_handler);
	sqlite3_exec(db_handler, db_cmd, NULL, NULL, NULL );
	sqlite3_close(db_handler);
}

/**********************************************************************
 * Photo Ctrl
 * 
 * 
 *********************************************************************/

/*
 * Enables the light and takes a photo
 */
void photo(const char *p)
{
	char cmd[128];

	led_light();	
	sprintf(cmd, "%s %s", PHOTO_CMD, p );
	//system( cmd );
	led_light( 0 );
}

/**********************************************************************
 * Sensor states
 * 
 * 
 *********************************************************************/
char inner_sensor_fail_enabled = 0;

void inner_sensor_raised()
{
	add_to_db( SENSOR_EVTYPE_INOUT );
	led_light();
	pipe_send_event( 0 );
}

void inner_sensor_enabled( unsigned long msec )
{
	if( msec > 60000 && inner_sensor_fail_enabled == 0 ){
		inner_sensor_fail_enabled = 1;
		logtext( LOG_FAIL_SENSOR, "Sensor enabled too long. There might be a sensor problem" );
	}
}

void inner_sensor_fallen()
{
		inner_sensor_fail_enabled = 0;
}

void inner_sensor_disabled( unsigned long msec )
{
}

void main_loop()
{
	int run = 1;

	static struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = TIMESLICE_NORM * MSEC;
	
	// Duration of an enabled and disabed state
	unsigned long duration_msec = 0;

	// Current and previous states of the input pins
	char pf_curr_state = 0;
	char pf_prev_state = 0;

	// Bitmasks for all inputs
	char pf_bm_quit = pfio_get_pin_bit_mask(IPIN_QUIT);
	char pf_bm_inner_sensor = pfio_get_pin_bit_mask(IPIN_INNER_SENSOR);
	char pf_bm_outer_sensor = pfio_get_pin_bit_mask(IPIN_OUTER_SENSOR);

	int photo_cnt = 0;

	init_duration();

	sensor();
	
	while (run){
		// Read all Piface Inputs
		pf_curr_state = pfio_read_input();

		// Check if inner sensor is not active. This will be the case, if
		// the light barrier is blocked (a bird is between LED and sensor)
		if( not_active( pf_curr_state, pf_bm_inner_sensor ) ){
			if( is_active( pf_prev_state, pf_bm_inner_sensor ) ){
					inner_sensor_raised();
					// Poll state faster
					ts.tv_nsec = TIMESLICE_FAST * MSEC;
					reset_duration( DURATION_INNER_SENSOR );
					//photo( PHOTO_PAR_SENSOR );
			}
			inner_sensor_enabled( get_duration( DURATION_INNER_SENSOR ) );
			inc_duration( DURATION_INNER_SENSOR, TIMESLICE_FAST );
		}else{
			if( not_active( pf_prev_state, pf_bm_inner_sensor ) ){
				ts.tv_nsec = TIMESLICE_NORM * MSEC;
				inner_sensor_fallen();
				reset_duration( DURATION_INNER_SENSOR );
				reset_duration( DURATION_WAIT_NEXT_PHOTO );
				photo_cnt = PHOTO_AFTER_EVENT_CNT;
			}
			inner_sensor_disabled( get_duration( DURATION_INNER_SENSOR ) );
			inc_duration( DURATION_INNER_SENSOR, TIMESLICE_NORM );
		}

		/*
		printf( "c: 0x%02x, p: 0x%02x", pf_curr_state, pf_prev_state );
		puts( "...	" );
		*/

		// Stop running, if Btn IPIN_QUIT was pressed
		if( is_active( pf_curr_state, pf_bm_quit ) ){
			run = 0;
		}

		if( photo_cnt > 0 ){
			if( is_timeout( DURATION_WAIT_NEXT_PHOTO, PHOTO_AFTER_EVENT_INTERVAL ) ){
				//photo( PHOTO_PAR_ENTERED);
				photo_cnt--;
				reset_duration( DURATION_WAIT_NEXT_PHOTO );
				if( photo_cnt == 0 ){
					led_light( 0 );
				}
			}

			inc_duration( DURATION_WAIT_NEXT_PHOTO, ts.tv_nsec / MSEC );
		}

		pf_prev_state = pf_curr_state;
		nanosleep( &ts, NULL );
	}

	sensor( 0 );
}




int main(int argc, char *argv[])
{
	// Init piface
	if (pfio_init() < 0){
		puts( "Could not initialize piface\n" );
		logtext( LOG_FAIL_SENSOR, "Could not initialize piface" );
		exit(-1);
	}

	// Disable all output ports
	pfio_write_output( 0x00 );
	
	// Enable Sensor
	pfio_digital_write( OPIN_IR_SENSOR, 1 );

	logtext( LOG_INFO_SENSOR, "Sensor initialized and waiting" );
	puts( "Waiting for the birds to come.\nPress Button on PiFace to stop" );

	// Initialize named pipe
	pipe_init();

	// This is the main loop
	main_loop();
	
	// Send exit on named pipe and close pipe
	pipe_send_event( 0xffff );
	pipe_close();


	// Disable all outputs and deinitialize
	pfio_write_output( 0x00 );
	pfio_deinit();

	logtext( LOG_INFO_SENSOR, "Sensor deinitilized" );
	puts("Bye" );
	return 0;
}
