/***********************************************************************
 * NAMES: Mostafa AlNaimi, Matthew Hagan, and Brian Yousif-Dickow
 * FILE: EtherCAT_app.c
 * PURPOSE: ECE5620 - Final Project
 * CONTENTS:  EtherCAT Master application to control a weather station.
 *            Linear Topology contains a master and two slave devices. 
 *            The sensors (Thermistor, Photocell, Humidity, and Pressure)
 *            will be connected to the Analog interfaces on the slave devices.
 *            Analog interfaces will be mapped and transfer to the master
 *            through EtherCAT communication. The application uses SOEM
 *            Open Source library for establishing the EtherCAT communication.
 *
 * PROJECT:   Weather Station Control using EtherCAT communication
 *
 * Usage : ./EtherCAT_app [ifname1]
 *
 * NOTES:
 *  (1) ifname is NIC interface, f.e. eth0
 *  (2) User could get ifname using Linux by ifconfig command
 *  (3) To close the application, Enter quit in the terminal
 *  (4) Measurements for all sensors will be shown on the screen
 *  (5) LEDs flashing only reflect Temperature and Light sensors
 *
 * KNOWN PROBLEMS:
 *
 * PORTABILITY CONCERNS:
 *
 * ENTRY POINTS (callable functions):
 *
 *      main( int argc, char *argv[] )
 *      ecat_init( ecat_master_drv_t *drvr_data_p )
 *      comm_thread( void *p )
 *      ecat_err_check( void *ptr )
 *      ecat_print( void *p )
 *      scale_sensors( uint16_t reading, int flag )
 *
 ***********************************************************************/

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include  <stdlib.h>
#include  <unistd.h>
#include  <sys/time.h>
#include  <time.h>
#include  <pthread.h>
#include  <sched.h>

#include "ethercat.h"

/* Define SUCCESS and FAIL will be used for status throughout the application */
#define SUCCESS 0
#define FAIL -1

/*
 * Reconfiguration Timeout in us
 */
#define  EC_TIMEOUTMON                          500

/*
 * Max I/O MAP SIZE 
 */
#define  MAX_IO_MAP                             2048

/*
 * Max SAFE_OP Tries 
 */
#define  MAX_SAFE_OP_TRY                        50

/*
 * Network interfaces 
 */
#define  MAX_ECAT_NIC_PORT                      2 
#define  MAX_ECAT_NIC_NAMELEN                   50 

/*
 * Slave Device Information
 */
#define  MAX_SLAVE_NAMELEN                      40 
#define  MAX_NUM_SLAVE                          20 

/* EtherCAT Error checking, Printing & Communication threads priorities*/
#define  ECAT_PRINT_THREAD_PRIO             20
#define  ECAT_ERR_CHECK_THREAD_PRIO         30
#define  ECAT_COMM_THREAD_PRIO              40

/* EtherCAT State Test Time in us */
#define ECAT_TEST_TIME                          500

/* Light Sensor Reading Scale */
#define LOW_RANGE  1000
#define HIGH_RANGE 3500

/* Temperature Sensor Reading Scale */
#define TEMP_LOW_RANGE  70.0
#define TEMP_MID_RANGE  74.0
#define TEMP_HIGH_RANGE 78.0

/* Sensors flags will be used for scaling function */
#define TEMP_SENSOR  1
#define LIGHT_SENSOR 2
#define PRESS_SENSOR 3
#define HUMID_SENSOR 4

/*
 * Data used by the driver for each instance of the driver.
 */
#define ERR_STR_LEN                 100

/* Define global structures will be used throughput the application. */
typedef struct ecat_slave ecat_slave_t;

typedef struct ecat_slave {
    char                slave_name[MAX_SLAVE_NAMELEN];   /* Slave name per ESI file */
    uint32_t            slave_id;      /* Slave ID per ESI file */
    int                 slave_index;   /* Index of the slave in the list */  
    char                *out_addr;     /* Pointer to the first byte of the output buffer */
    char                *in_addr;      /* Pointer to the first byte of the input buffer */
    int                 curr_state;   /* Slave current state */
    int                 req_state;    /* Slave requested state */
    float               temp_sensor;  /* Reading temperature sensor */
    float               light_sensor; /* Reading photocell sensor */
    float               press_sensor; /* Reading pressure sensor */
    float               humd_sensor;  /* Reading humdity sensor */
    char                leds;         /* Writing LED values */
    char                buttons;      /* Reading buttons values */
    uint16_t            sensor_data;
    uint16_t            sensor_data1;
    int                 sm;            /* Number of SyncManagers configured */
    int                 fmmu;          /* Number of FMMUs configured */
    int                 dc;            /* Flag if slave supports Distributed Clock */
    int                 lost_connection; /* Flag is set when slave lose connection */
    int                 rx_size;       /* RX PDOs size */
    int                 tx_size;        /* TX PDOs size */
} ecat_slave;

typedef struct {

    int              status; /* Status variable to control the application */
    int              redundant_mode; /* Copy of Application Param */
    char             err_detail[ERR_STR_LEN + 1]; /* for file parsing errors */
    char             nic_port[MAX_ECAT_NIC_NAMELEN];   /* NIC port name */ 
    char             nic_red_port[MAX_ECAT_NIC_NAMELEN];   /* NIC port name */    
    pthread_t        errcheck_tid;  /* EtherCAT error check thread */
    pthread_t        printing_tid;  /* EtherCAT error check thread */
    pthread_t        rcv_tid;     /* EtherCAT communication thread */
    int              op_flag;      /* Operational state flag */
    volatile int     wkc;       /* Working Counter = Number of Slaves found */
    volatile int     expectedWKC; /* Expected Working Counter when frame is received */
    char             IOmap[MAX_IO_MAP]; /* Pointer to the I/O MAP */
    int              IOmap_size;  /* Size of I/O MAP for all Slaves*/
    ecat_slave       *targets[MAX_NUM_SLAVE];   /* Copy of slaves structure */
    int              num_slaves; /* Number of slaves configured */
    double           curr_time;  /* Current Clock Time */
    double           prev_time;  /* Previous Clock Time */
    struct           timeval tv; /* Time spec */
    int              read_wkc;  /* Monitor Working Counter */
    unsigned long    SimCounter;  /* Counter to keep track of running cycles */
    
} ecat_master_drv_t;

static char exp_exit_cmd[]="quit";

/* Define function calls and threads will be used in the application */
static int ecat_init(ecat_master_drv_t *drvr_data_p);
static int ecat_app_close(ecat_master_drv_t *drvr_data_p);
static void *comm_thread(void *p);
static void *ecat_err_check( void *p );
static void *ecat_print( void *p );
static float scale_sensors( uint16_t reading, int flag );

/***************************************************************************
 *  scale_sensors()
 *
 *     This function will be used to scale the sensors from the
 *     raw reading values.
 *
 *************************************************************************/
static float scale_sensors( uint16_t reading, int flag )
{
    /* Defining Constants for Conversion & temp variables */
    float R1 = 100000;
    float logR2, R2, T;
    float c1 = 1.009249522e-03, c2 = 2.378405444e-04, c3 = 2.019202697e-07;
    float humdity_const = 0.0048875;
    const int zero = 102, span =  819;
    float humdity_conversion;
    float pressure;
    
    /* Scaling Temperature sensor analog values */
    if (flag == TEMP_SENSOR){
        R2 = R1 * (4095.0 / (float)reading - 1.0);
        logR2 = log(R2);
        T = (1.0 / (c1 + c2*logR2 + c3*logR2*logR2*logR2));
        T = T - 273.15;
        T = (T * 9.0)/ 5.0 + 32.0;
        T *= -1 ;
        return T;
    }
    
    /* Scaling Pressure sensor analog values */
    else if (flag == PRESS_SENSOR){
        pressure = (reading - zero)  * 15.0 / span;
        return pressure;
    }
    
    /* Scaling Humidity sensor analog values */
    else if (flag == HUMID_SENSOR){
        humdity_conversion = humdity_const * (float)reading;
        return (float)((humdity_conversion-0.86)/0.03);
    }

    return SUCCESS;
}
/*************************************************************************
 *  ecat_print()
 *
 *     This thread handles printing EtherCAT information, Slave(s) info,
 *     and all necessary counters and Data.
 *
 *************************************************************************/
static void *ecat_print( void *p )
{
    ecat_master_drv_t *drvr_data_p;
    
    /* Copy local data structure */
    drvr_data_p = (ecat_master_drv_t *) p;
    
    while (1){
        
        /* Print sensors and cycle information */
        printf("Num of Slaves(s):%d ", drvr_data_p->num_slaves); /* Number of slaves */
        printf(" | Temp:%f | Light:%f ", drvr_data_p->targets[0]->temp_sensor,
            drvr_data_p->targets[0]->light_sensor); /* Temperature and Light sensors */
        printf(" | Pressure:%f | Humidity:%f ", drvr_data_p->targets[1]->press_sensor,
            drvr_data_p->targets[1]->humd_sensor); /* Pressure & Humidity sensors */
        printf(" | Num cycles:%ld\r", drvr_data_p->SimCounter); /* Cycle count */
                
    /* Sleep 100ms */
    osal_usleep( 100000 );
    }
    return NULL;
}
/*************************************************************************
 *  ecat_err_check()
 *
 *     This thread handles monitor EtherCAT state machine. Take actions
 *     upon found errors and bad states. The thread sleeps for only 10ms 
 *
 *************************************************************************/
static void *ecat_err_check( void *p )
{
    ecat_slave     *target;
    int            safe_count, i, num_err_slave;
    ecat_master_drv_t *drvr_data_p;
    
    /* Copy local data structure */
    drvr_data_p = (ecat_master_drv_t *) p;
    
    /* Initialize local variable */
    safe_count = 0;
    num_err_slave = 0;

    while(1)
    {
        /* Monitor the state machine of each slave */
        for(i = 0; i < drvr_data_p->num_slaves; i++){
            target = drvr_data_p->targets[i];
            if (target != NULL){
                target->curr_state = ec_statecheck(target->slave_index, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                /* If the state is not operational then set lost connection flag */
                if (target->curr_state != EC_STATE_OPERATIONAL) target->lost_connection = 1;
            }
            /* Count how many slaves are lost connection */
            if (target->lost_connection){
                num_err_slave++;
            }
        }        
        
        /*
         * Verify working counter.
         * Check if all slaves are not in operational mode. 
         */
        if(drvr_data_p->wkc < drvr_data_p->expectedWKC && num_err_slave == drvr_data_p->num_slaves)
        {
            /* One or more slaves are not responding */
            for (i = 0; i < drvr_data_p->num_slaves; i++){
                
                target = drvr_data_p->targets[i];   
                if (target != NULL){
                    /* Slave is not in OPERATIONAL mode */
                    if ( ec_slave[target->slave_index].state != EC_STATE_OPERATIONAL ) {
                            
                        /* Attempting ACK and set to Init state */
                        if (ec_slave[target->slave_index].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
                        {
                            /* Acknowledge error and set the state to Init */
                            printf("ERROR : slave %d is in SAFE_OP + ERROR, attempting ack.\n",
                                        target->slave_index);
                            ec_slave[target->slave_index].state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
                            ec_writestate(target->slave_index);
                            ec_slave[target->slave_index].state = (EC_STATE_INIT);
                            ec_writestate(target->slave_index);
                        }
                        /* Slave is in SAFE_OP. Try to switch to OPERATIONAL state */
                        else if(ec_slave[target->slave_index].state == EC_STATE_SAFE_OP)
                        {
                            /* When maximum tries take a place, switch to init state */
                            if ( safe_count >= MAX_SAFE_OP_TRY ){
                                ec_slave[target->slave_index].state = (EC_STATE_INIT);
                                ec_writestate(target->slave_index);
                                printf("WARNING : slave %d exceeded MAX Number for SAFE_OP. Switch to Init\n",
                                            target->slave_index);
                                drvr_data_p->op_flag = 0;
                            }
                            else{
                                /* If the slave is in safe operational mode, then switch to Operational */
                                printf("WARNING : slave %d is in SAFE_OP, change to OPERATIONAL.\n",
                                            target->slave_index);
                                ec_slave[target->slave_index].state = EC_STATE_OPERATIONAL;
                                ec_writestate(target->slave_index);
                                target->lost_connection = 0;
                                safe_count = safe_count + 1;
                            }
                        }
                            
                        /* Try to re-config Slave */
                        else if(ec_slave[target->slave_index].state > EC_STATE_NONE)
                        {
                            if (ec_reconfig_slave(target->slave_index, EC_TIMEOUTMON))
                            {
                                ec_slave[target->slave_index].islost = FALSE;
                                printf("Slave %d is reconfigured\n",target->slave_index);
                            }
                        }
                        else if(!ec_slave[target->slave_index].islost)
                        {
                            /* Slave is lost and re-checking the state */
                            ec_statecheck(target->slave_index, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                            if (ec_slave[target->slave_index].state == EC_STATE_NONE)
                            {
                                ec_slave[target->slave_index].islost = TRUE;
                                printf("ERROR : slave %d lost\n",target->slave_index);
                            }
                        }
                    }
                    /* Can't find the slave on the network, totally lost */
                    if (ec_slave[target->slave_index].islost)
                    {
                        if(ec_slave[target->slave_index].state == EC_STATE_NONE)
                        {
                            if (ec_recover_slave(target->slave_index, EC_TIMEOUTMON))
                            {
                                ec_slave[target->slave_index].islost = FALSE;
                                target->lost_connection = 0;
                                printf("MESSAGE : slave %d recovered\n",target->slave_index);
                            }
                        }
                        else
                       {
                            ec_slave[target->slave_index].islost = FALSE;
                            target->lost_connection = 0;
                            printf("MESSAGE : slave %d found\n",target->slave_index);
                        }
                    }
                }
            }
        }
        /* Sleep 10ms */
        osal_usleep( 10000 );
    }    
    
    return NULL;
}
/**********************************************************************
 *  comm_thread()
 *
 *     This thread handles EtherCAT Communication for PDOs read/write.
 *     The thread sleeps only for 5 milliseconds time.
 *
 **********************************************************************/
static void *comm_thread(void *p)
{
    ecat_slave     *target;
    int i;
    ecat_master_drv_t *drvr_data_p;
    char *data_ptr;
    char *data_ptr1;
    char led_data;

    /* Copy application data structure */
    drvr_data_p = (ecat_master_drv_t *) p;

    /*
     * Monitor Operational flag for slave(s) during runtime.
     */
    while ( drvr_data_p->op_flag ){

        /*
         * Do the write for PDOs only when Slave is in Operational mode.
         */                    
        for (i = 0; i < drvr_data_p->num_slaves; i++){
            target = drvr_data_p->targets[i];
            if (target != NULL && ec_slave[target->slave_index].state == EC_STATE_OPERATIONAL){
                /* Write Data to PDOs */
                if (target->slave_index == 1){

                    /* Scale light sensor raw data based on 3 intervals */
                    /* High range interval - Bright */
                    if (target->light_sensor > HIGH_RANGE){
                        led_data = 0xff;
                        memcpy(drvr_data_p->targets[1]->out_addr, &led_data, sizeof(char));
                    }
                    /* Mid range interval - Normal room light */
                    else if (target->light_sensor > LOW_RANGE && target->light_sensor < HIGH_RANGE){
                        led_data = 0x0f;
                        memcpy(drvr_data_p->targets[1]->out_addr, &led_data, sizeof(char));
                    }
                    /* Low range interval - Dark */
                    else{
                        led_data = 0x00;
                        memcpy(drvr_data_p->targets[1]->out_addr, &led_data, sizeof(char));
                    }
                    
                    /* Scale Temperature values based on 4 intervals */
                    /* Room Temperature */
                    if (target->temp_sensor > TEMP_LOW_RANGE && target->temp_sensor < TEMP_MID_RANGE){
                        led_data = 0x07;
                        memcpy(target->out_addr, &led_data, sizeof(char));
                    }
                    /* Nice weather Temperature */
                    else if (target->temp_sensor > TEMP_MID_RANGE && target->temp_sensor < TEMP_HIGH_RANGE){
                        led_data = 0x1F;
                        memcpy(target->out_addr, &led_data, sizeof(char));
                    }
                    /* Warm Temperature */
                    else if (target->temp_sensor > TEMP_HIGH_RANGE){
                        led_data = 0xFF;
                        memcpy(target->out_addr, &led_data, sizeof(char));
                    }
                    /* Cold Temperature */
                    else if (target->temp_sensor < TEMP_LOW_RANGE) {
                        led_data = 0x00;
                        memcpy(target->out_addr, &led_data, sizeof(char));
                    }
                }
            } 
        }
        /* Transmit Process Data for PDOs */
        ec_send_processdata();
        
        /* Receive Process Data for PDOs*/
        drvr_data_p->wkc = ec_receive_processdata(EC_TIMEOUTRET);
        
        /*
         * Do the read for PDOs only when Slave is in Operational mode.
         */                    
        for (i = 0; i < drvr_data_p->num_slaves; i++){
            target = drvr_data_p->targets[i];
            if (target != NULL && ec_slave[target->slave_index].state == EC_STATE_OPERATIONAL){
                /* For slave 1, copy reading for Light and Temperature sensors */
                if (target->slave_index == 1){
                    
                    data_ptr = target->in_addr;
                    memcpy(&target->sensor_data, data_ptr, sizeof(uint16_t));
                    data_ptr += sizeof(uint16_t);
                    memcpy(&target->sensor_data1, data_ptr, sizeof(uint16_t));
                    data_ptr += sizeof(uint16_t);
                    memcpy(&target->buttons, data_ptr, sizeof(char));
                    
                    target->light_sensor = (float) (target->sensor_data);
                    /* Convert temperature reading scale */
                    target->temp_sensor = scale_sensors(target->sensor_data1, TEMP_SENSOR);
                }
                
                /* For slave 2, copy reading for Pressure and Humidity sensors */
                else if (target->slave_index == 2){
                    
                    data_ptr1 = target->in_addr;
                    memcpy(&target->sensor_data, data_ptr1, sizeof(uint16_t));
                    data_ptr1 += sizeof(uint16_t);
                    memcpy(&target->sensor_data1, data_ptr1, sizeof(uint16_t));
                    data_ptr1 += sizeof(uint16_t);
                    memcpy(&target->buttons, data_ptr1, sizeof(char));
                    
                    /* Convert Pressure & Humidity */
                    target->press_sensor = scale_sensors(target->sensor_data, PRESS_SENSOR);
                    target->humd_sensor = scale_sensors(target->sensor_data1, HUMID_SENSOR);
                }
            } 
        }
        /*Increment Simulation Counter */
        drvr_data_p->SimCounter = drvr_data_p->SimCounter + 1;
        
        /* Sleep for 5ms */
        osal_usleep( 5000 );
    }
    
    return (NULL);
}                               /* comm_thread */

/**************************************************************************
 *
 *  ecat_app_close() - Request all slave(s) to Init state, close EtherCAT 
 *      socket connection on NIC port, free all driver data structures,
 *      and cancel all threads.
 *
 **************************************************************************/
int ecat_app_close(ecat_master_drv_t *drvr_data_p)
{
    ecat_slave *target;
    int i;
    
    /* Request Init State for all Slaves */
    drvr_data_p->op_flag = 0;
    ec_slave[0].state = EC_STATE_INIT;
    ec_writestate(0);
    
    /* stop SOEM, close socket */
    ec_close();
    
    /* Cancel all active threads */
    if ( drvr_data_p->rcv_tid ){
        
        pthread_cancel(drvr_data_p->printing_tid);
        usleep(5000);
        pthread_cancel(drvr_data_p->errcheck_tid);
        usleep(5000);
        pthread_cancel(drvr_data_p->rcv_tid);
        usleep(5000);
    }
    
    /* Free slave(s) data structure */
    for(i = 0; i < drvr_data_p->num_slaves; i++){
        target = drvr_data_p->targets[i];
        if (target != NULL)
            free(target);
    }
    
    /* Free application data structure */
    free(drvr_data_p);
    
    return SUCCESS;
    
}
/**************************************************************************
 *
 *  ecat_init() - Initialize NIC port for EtherCAT, Configure Slaves 
 *                and I/O MAP for PDOs, and Request OP state for Slaves.
 *
 **************************************************************************/
int ecat_init ( ecat_master_drv_t *drvr_data_p ){
    
    ecat_slave *target;
    ec_adaptert *ret_adapter;
    int i, status, states, adapter_found, red_adapter_found;

    /* Initialize local variable */
    adapter_found = 0;
    red_adapter_found = 0;

    /* 
     * Find available adapters on the system.
     * Compare Found adapters with the passed NIC port.
     */
    ret_adapter = ec_find_adapters();
    if ( ret_adapter == NULL ){
        printf("Can't find available network adapters on the system\n" );
        return FAIL;
    }
        
    /* Match Network adapter with the one passed through the argument */
    for ( ; ret_adapter != NULL; ret_adapter = ret_adapter->next ){
                                
        if ( strcmp(drvr_data_p->nic_port, ret_adapter->name) == 0) {
            adapter_found = 1;
            break;
        }
    }
    
    /* Couldn't find matched network adapters */
    if ( !adapter_found ) {
        printf("Can't find matched network adapter\n" );
        return FAIL;
    }
    
    /* 
     * Check for redundancy port
     * Not supported in this application
     */
    if ( drvr_data_p->redundant_mode ){
        ret_adapter = ec_find_adapters();
        for ( ; ret_adapter != NULL; ret_adapter = ret_adapter->next ){
                                
            if ( strcmp(drvr_data_p->nic_red_port, ret_adapter->name) == 0) {
                red_adapter_found = 1;
                break;
            }
        }
        
        /* No redundant network adapter was found */
        if ( !red_adapter_found ) {
            printf("Can't find matched redundant network adapter\n" );
            return FAIL;
        } 
    }
       
    /* 
     * Initialize and bind socket to nic_port on a single port, or
     * check for redundant mode (second port).
     */ 
    if ( drvr_data_p->redundant_mode ){
        status = ec_init_redundant( drvr_data_p->nic_port, 
                                    drvr_data_p->nic_red_port );
    }
    
    /* Single NIC mode */
    else {
        status = ec_init( drvr_data_p->nic_port ); 
    }
    
    /* 
     * Continue to discover and configure slave(s) if
     * Socket connection of the EtherCAT was established successfully.
     */
    if ( status )
    {
        /* SOEM init is done and NIC is up */
        
        /*
         * Broadcast read request to all functional slaves in the network.
         * Update Working Counter and sets up Mailboxes. Request PRE OP state.
         */
        if ( ( drvr_data_p->wkc = ec_config_init(FALSE) ) > 0 ){

            /* 
             * Copy Slave name, Slave ID, and store Slave index
             * Store Slave information (FMMU, Distributed Clock, and number of SyncManger)
             */
            drvr_data_p->num_slaves = ec_slavecount;
            i = 1;
            while ( i <= ec_slavecount ){
                /* Allocate Slave data structure */
                target = (ecat_slave *) calloc(1, sizeof(ecat_slave));
                if (target == NULL){
                    printf("Can't allocate slave data structure\n");
                    return FAIL;
                }
                /* Slave index */
                target->slave_index = i;
                /* Slave ID */
                target->slave_id = ec_slave[i].eep_man;
                /* Copy Slave name */
                strncpy(target->slave_name, ec_slave[i].name, strlen(ec_slave[i].name));
                /* Check if Slave has a distributed clock */
                target->dc = ec_slave[i].hasdc;
                /* Store slave structure in the local application structure */
                drvr_data_p->targets[i-1] = target;
                i++;
            }
            
            /* Can't find slave(s) devices in the network */
            if ( drvr_data_p->num_slaves < 1 ){
                
                printf(drvr_data_p->err_detail, "Can't find EtherCAT Slave(s) in the network\n");
                return FAIL;
            }
            
            /* Request all slaves to reach PRE_OP state */
            states = ec_statecheck(0, EC_STATE_PRE_OP,  EC_TIMEOUTSTATE * 4);
            if ( states != EC_STATE_PRE_OP ){
                
                /* Slave(s) can't reach Pre operational state */
                printf("Slaves can't reach PRE_OP state\n" );
                return FAIL;
            }
            
            /*
             * Create I/O MAP for slaves. Configure SyncMangers and FMMUs.
             * Update expected Working Counter and request SAFE_OP state.
             */
            drvr_data_p->IOmap_size = ec_config_map(&(drvr_data_p->IOmap));
            
            if (!(drvr_data_p->IOmap_size)){
                printf("Can't create IO map for PDOs\n");
                return FAIL;
            }
            
            /* Store copies of the I/O address will be accessed to write/read PDOs */
            for ( i = 0; i < drvr_data_p->num_slaves; i++){
                        
                target = drvr_data_p->targets[i];
                target->out_addr = (char *) ec_slave[target->slave_index].outputs;
                target->in_addr = (char *) ec_slave[target->slave_index].inputs;
                
                /* Copy number of bytes of TX PDOs */
                if (ec_slave[target->slave_index].Obytes)
                    target->tx_size = ec_slave[target->slave_index].Obytes;
                /* Num of bits instead */
                else
                    target->tx_size = 1;
                
                /* Copy number of bytes of RX PDOs */
                if (ec_slave[target->slave_index].Ibytes)
                    target->rx_size = ec_slave[target->slave_index].Ibytes;
                /* Num of bits instead */
                else
                    target->rx_size = 1;
            }
            /* Configure Distributed Clock it's used in any slave */                      
            ec_configdc();

            /* Request all slaves to reach SAFE_OP state */
            states = ec_statecheck(0, EC_STATE_SAFE_OP,  EC_TIMEOUTSTATE * 4);
            if ( states != EC_STATE_SAFE_OP ){
                
                printf("Slaves can't reach SAFE_OP state\n" );
                return FAIL;
            }
              
            /* Calculate expected working counter for all slaves in the network */
            drvr_data_p->expectedWKC = (ec_group[0].outputsWKC * 2) + \
                ec_group[0].inputsWKC;
            
            for ( i = 0; i < drvr_data_p->num_slaves; i++ ){
                        
                target = drvr_data_p->targets[i];
                
                if (target != NULL){
                    /* Set Current state for available slaves */
                    target->curr_state = states;
                    
                    /* request OP state for all slaves */
                    ec_slave[target->slave_index].state = EC_STATE_OPERATIONAL;
                    ec_writestate(target->slave_index);
                    
                    /* send one valid process data to keep slaves happy */
                    ec_send_processdata();
                    ec_receive_processdata(EC_TIMEOUTRET);

                    if (ec_slave[target->slave_index].state != EC_STATE_OPERATIONAL){
                    
                        printf("Slave %d can't reach OP state\n", target->slave_index);
                        return FAIL;
                    }
                    /* Set Current state  to Operational */                    
                    target->curr_state = EC_STATE_OPERATIONAL;
                }
            }
            
            /* Set Operational Flag */
            drvr_data_p->op_flag = TRUE;
            /* Read system clock */
            gettimeofday(&(drvr_data_p->tv), NULL);
            drvr_data_p->prev_time = (double)drvr_data_p->tv.tv_sec + (double)drvr_data_p->tv.tv_usec/1000000.0;
        }
        
        else {
            /* No slave(s) is/are found in the network */
            printf("Can't find Slave(s) in the network\n" );
            return FAIL;
        }    
    }
    /* Couldn't establish EtherCAT socket on the NIC port(s) */
    else {
        printf("Can't connect to NIC port(s)\n" );
        return FAIL;
    }
    return SUCCESS; 
}

/*******************************************************************************
 *
 * int main(int argc, char *argv[])
 *
 *   Main function that will drive the Master application.
 *
 ******************************************************************************/
int main(int argc, char *argv[])
{
    pthread_attr_t  attr;
    struct sched_param   sched_param;
    ecat_master_drv_t *drvr_data_p;
    char exit_cmd[20];
   
    printf("Weather Station - EtherCAT Master application\n\n");
    printf ("Enter: 'quit' to exit the application\n\n");

    /* Allocate application data structure will be used throughout the application */ 
    drvr_data_p = (ecat_master_drv_t *) calloc(1, sizeof(ecat_master_drv_t));
    
    if (drvr_data_p == NULL)
    {
        printf("EtherCAT_app FAIL. Couldn't allocate drvr_data_p structure\n");
        return FAIL;
    }
    
    /* Copy NIC port name passed through the argument when running the application */
    if (argc > 1)
    {
        /* Copy NIC port name to local data structure */
        strncpy(drvr_data_p->nic_port, argv[1], strlen(argv[1]));
        printf("Passed NIC port name = %s \n", drvr_data_p->nic_port);
        if (drvr_data_p->nic_port == NULL){
            printf("EtherCAT_app FAIL. Couldn't copy NIC port name\n");
            return FAIL;
        }
    }
    else
    {
        printf("EtherCAT_app FAIL. No NIC name was passed\n");
        printf("Usage: ./EtherCAT_app [ifname], ifname = eth0 for example\n");
        return FAIL;
    }
   
    /*
     * Call ecat_init() to establish, initialize, discover and
     * set all slave(s) configuration. Move slave(s) to operational state.
     */
    drvr_data_p->status = ecat_init(drvr_data_p);
    
    if (drvr_data_p->status < SUCCESS)
    {
        printf("EtherCAT_app FAIL. EtherCAT Master initialization has failed\n");
        return FAIL;
    }
    
    /*
     * Create threads for error checking and Communication Cycle.
     */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_getschedparam(&attr, &sched_param);
    
    /* Set priority and create communication thread */
    sched_param.sched_priority = ECAT_COMM_THREAD_PRIO;
    pthread_attr_setschedparam(&attr, &sched_param);
    pthread_create(&(drvr_data_p->rcv_tid), &attr,
                comm_thread, (void *) drvr_data_p);
    
    /* Set priority and create error monitor thread */
    sched_param.sched_priority = ECAT_ERR_CHECK_THREAD_PRIO;
    pthread_attr_setschedparam(&attr, &sched_param);
    pthread_create(&(drvr_data_p->errcheck_tid), &attr,
                ecat_err_check, (void *) drvr_data_p);
    
    /* Set priority and create Printing thread */
    sched_param.sched_priority = ECAT_PRINT_THREAD_PRIO;
    pthread_attr_setschedparam(&attr, &sched_param);
    pthread_create(&(drvr_data_p->printing_tid), &attr,
                ecat_print, (void *) drvr_data_p);
    
    /* Wait until user inserts quit to exit out of the application */
    while (1){
        scanf("%s", exit_cmd);
        if (strncmp(exit_cmd, exp_exit_cmd, strlen(exp_exit_cmd)) == 0) break;
    }
    
    /* Close application, EtherCAT socket */
    drvr_data_p->status = ecat_app_close(drvr_data_p);
    if (drvr_data_p->status < SUCCESS){
        printf("EtherCAT_app FAIL. ecat_app_close()has failed\n");
        return FAIL;
    }

    printf("\nEtherCAT_app program end\n");
    return SUCCESS;
}
