#include "pigfx_config.h"
#include "uart.h"
#include "utils.h"
#include "timer.h"
#include "framebuffer.h"
#include "console.h"
#include "gfx.h"
#include "irq.h"
#include "dma.h"
#include "nmalloc.h"
#include "ee_printf.h"
#include "../uspi/include/uspi.h"


#define GPFSEL1 0x20200004
#define GPSET0  0x2020001C
#define GPCLR0  0x20200028

#define UART_BUFFER_SIZE 16384 /* 16k */


unsigned int led_status;
volatile unsigned int* UART0_DR;
volatile unsigned int* UART0_ITCR;
volatile unsigned int* UART0_IMSC;
volatile unsigned int* UART0_FR;


volatile char* uart_buffer;
volatile char* uart_buffer_start;
volatile char* uart_buffer_end;
volatile char* uart_buffer_limit;

extern unsigned int pheap_space;
extern unsigned int heap_sz;

#if ENABLED(SKIP_BACKSPACE_ECHO)
volatile unsigned int backspace_n_skip;
volatile unsigned int last_backspace_t;
#endif


static void _keypress_handler(const char* str )
{
    const char* c = str;
    char CR = 13;

    while( *c )
    {
         char ch = *c;
         //ee_printf("CHAR 0x%x\n",ch );

#if ENABLED(SEND_CR_ONLY)
        // DR: PianoMan's CR only fix
	if( ch == 10 )
	{
		ch = 13;
	}
#endif

#if ENABLED(SEND_CR_LF)
        if( ch == 10 )
        {
            // Send CR first
            uart_write( &CR, 1 );

        }
#endif

#if ENABLED( SWAP_DEL_WITH_BACKSPACE )
        if( ch == 0x7F ) 
        {
            ch = 0x8;
        }
#endif

#if ENABLED( BACKSPACE_ECHO )
        if( ch == 0x8 )
            gfx_term_putstring( "\x7F" );
#endif

#if ENABLED(SKIP_BACKSPACE_ECHO)
        if( ch == 0x7F )
        {
            backspace_n_skip = 2;
            last_backspace_t = time_microsec();
        }
#endif
        uart_write( &ch, 1 ); 
        ++c;
    }

} 


static void _heartbeat_timer_handler( __attribute__((unused)) unsigned hnd, 
                                      __attribute__((unused)) void* pParam, 
                                      __attribute__((unused)) void *pContext )
{
    if( led_status )
    {
        W32(GPCLR0,1<<16);
        led_status = 0;
    } else
    {
        W32(GPSET0,1<<16);
        led_status = 1;
    }

    attach_timer_handler( HEARTBEAT_FREQUENCY, _heartbeat_timer_handler, 0, 0 );
}


void uart_fill_queue( __attribute__((unused)) void* data )
{
    while( !( *UART0_FR & 0x10)/*uart_poll()*/)
    {
        *uart_buffer_end++ = (char)( *UART0_DR & 0xFF /*uart_read_byte()*/);

        if( uart_buffer_end >= uart_buffer_limit )
           uart_buffer_end = uart_buffer; 

        if( uart_buffer_end == uart_buffer_start )
        {
            uart_buffer_start++;
            if( uart_buffer_start >= uart_buffer_limit )
                uart_buffer_start = uart_buffer; 
        }
    }

    /* Clear UART0 interrupts */
    *UART0_ITCR = 0xFFFFFFFF;
}



void initialize_uart_irq()
{
    uart_buffer_start = uart_buffer_end = uart_buffer;
    uart_buffer_limit = &( uart_buffer[ UART_BUFFER_SIZE ] );

    UART0_DR   = (volatile unsigned int*)0x20201000;
    UART0_IMSC = (volatile unsigned int*)0x20201038;
    UART0_ITCR = (volatile unsigned int*)0x20201044;
    UART0_FR   = (volatile unsigned int*)0x20201018;

    *UART0_IMSC = (1<<4) | (1<<7) | (1<<9); // Masked interrupts: RXIM + FEIM + BEIM (See pag 188 of BCM2835 datasheet)
    *UART0_ITCR = 0xFFFFFFFF; // Clear UART0 interrupts

    pIRQController->Enable_IRQs_2 = RPI_UART_INTERRUPT_IRQ;
    enable_irq();
    irq_attach_handler( 57, uart_fill_queue, 0 );
}


void heartbeat_init()
{
    unsigned int ra;
    ra=R32(GPFSEL1);
    ra&=~(7<<18);
    ra|=1<<18;
    W32(GPFSEL1,ra);

    // Enable JTAG pins
    W32( 0x20200000, 0x04a020 );
    W32( 0x20200008, 0x65b6c0 );

    led_status=0;
}


void heartbeat_loop()
{
    unsigned int last_time = 0;
    unsigned int curr_time;

    while(1)
    {
        
        curr_time = time_microsec();
        if( curr_time-last_time > 500000 )
        {
            if( led_status )
            {
                W32(GPCLR0,1<<16);
                led_status = 0;
            } else
            {
                W32(GPSET0,1<<16);
                led_status = 1;
            }
            last_time = curr_time;
        } 
    }
}


void initialize_framebuffer()
{
    usleep(10000);
    fb_release();

    unsigned char* p_fb=0;
    unsigned int fbsize;
    unsigned int pitch;

    unsigned int p_w = 640;
    unsigned int p_h = 480;
    unsigned int v_w = p_w;
    unsigned int v_h = p_h;

    fb_init( p_w, p_h, 
             v_w, v_h,
             8, 
             (void*)&p_fb, 
             &fbsize, 
             &pitch );

    fb_set_xterm_palette();

    //cout("fb addr: ");cout_h((unsigned int)p_fb);cout_endl();
    //cout("fb size: ");cout_d((unsigned int)fbsize);cout(" bytes");cout_endl();
    //cout("  pitch: ");cout_d((unsigned int)pitch);cout_endl();

    if( fb_get_phisical_buffer_size( &p_w, &p_h ) != FB_SUCCESS )
    {
        //cout("fb_get_phisical_buffer_size error");cout_endl();
    }
    //cout("phisical fb size: "); cout_d(p_w); cout("x"); cout_d(p_h); cout_endl();

    usleep(10000);
    gfx_set_env( p_fb, v_w, v_h, pitch, fbsize ); 
    gfx_clear();
}


void video_test()
{
    unsigned char ch='A';
    unsigned int row=0;
    unsigned int col=0;
    unsigned int term_cols, term_rows;
    gfx_get_term_size( &term_rows, &term_cols );

#if 0
    unsigned int t0 = time_microsec();
    for( row=0; row<1000000; ++row )
    {
        gfx_putc(0,col,ch);
    }
    t0 = time_microsec()-t0;
    cout("T: ");cout_d(t0);cout_endl();
    return;
#endif
#if 0
    while(1)
    {
        gfx_putc(row,col,ch);
        col = col+1;
        if( col >= term_cols )
        {
            usleep(100000);
            col=0;
            gfx_scroll_up(8);
        }
        ++ch;
        gfx_set_fg( ch );
    }
#endif
#if 1
    while(1)
    {
        gfx_putc(row,col,ch);
        col = col+1;
        if( col >= term_cols )
        {
            usleep(50000);
            col=0;
            row++;
            if( row >= term_rows )
            {
                row=term_rows-1;
                int i;
                for(i=0;i<10;++i)
                {
                    usleep(500000);
                    gfx_scroll_down(8);
                    gfx_set_bg( i );
                }
                usleep(1000000);
                gfx_clear();
                return;
            }

        }
        ++ch;
        gfx_set_fg( ch );
    }
#endif

#if 0
    while(1)
    {
        gfx_set_bg( RR );
        gfx_clear();
        RR = (RR+1)%16;
        usleep(1000000);
    }
#endif

}


void video_line_test()
{
    int x=-10; 
    int y=-10;
    int vx=1;
    int vy=0;

    gfx_set_fg( 15 );

    while(1)
    {
        // Render line
        gfx_line( 320, 240, x, y );

        usleep( 1000 );

        // Clear line
        gfx_swap_fg_bg();
        gfx_line( 320, 240, x, y );
        gfx_swap_fg_bg();

        x = x+vx;
        y = y+vy;
        
        if( x>700 )
        {
            x--;
            vx--;
            vy++;
        }
        if( y>500 )
        {
            y--;
            vx--;
            vy--;
        }
        if( x<-10 )
        {
            x++;
            vx++;
            vy--;
        }
        if( y<-10 )
        {
            y++;
            vx++;
            vy++;
        }

    }
}


void term_main_loop()
{
    ee_printf("Waiting for UART data (115200,8,N,1)\n");

    /**/
    while( uart_buffer_start == uart_buffer_end )
        usleep(100000 );
    /**/

    gfx_term_putstring( "\x1B[2J", 0 );

    char strb[2] = {0,0};

    while(1)
    {
        if( !DMA_CHAN0_BUSY && uart_buffer_start != uart_buffer_end )
        {
            strb[0] = *uart_buffer_start++;
            if( uart_buffer_start >= uart_buffer_limit )
                uart_buffer_start = uart_buffer;

            
#if ENABLED(SKIP_BACKSPACE_ECHO)
            if( time_microsec()-last_backspace_t > 50000 )
                backspace_n_skip=0;

            if( backspace_n_skip  > 0 )
            {
                //ee_printf("Skip %c",strb[0]);
                strb[0]=0; // Skip this char
                backspace_n_skip--;
                if( backspace_n_skip == 0)
                    strb[0]=0x7F; // Add backspace instead
            }
#endif

            gfx_term_putstring( strb, NO_IMPLICIT_CR ); // DR: No implicit CR before LR
        }

        uart_fill_queue(0);
        timer_poll();
    }

}


void entry_point()
{
    // Heap init
    nmalloc_set_memory_area( (unsigned char*)( pheap_space ), heap_sz );

    // UART buffer allocation
    uart_buffer = (volatile char*)nmalloc_malloc( UART_BUFFER_SIZE ); 
    
    uart_init();
    heartbeat_init();
    
    //heartbeat_loop();
    
    initialize_framebuffer();

    gfx_term_putstring( "\x1B[2J", 0 ); // Clear screen
    gfx_set_bg(27);
    gfx_term_putstring( "\x1B[2K", 0 ); // Render blue line at top
    ee_printf(" ===  PiGFX ===  v.%s\n", PIGFX_VERSION );
    gfx_term_putstring( "\x1B[2K", 0 );
    //gfx_term_putstring( "\x1B[2K" ); 
    ee_printf(" Copyright (c) 2016 Filippo Bergamasco\n\n");
    gfx_set_bg(0);

    timers_init();
    attach_timer_handler( HEARTBEAT_FREQUENCY, _heartbeat_timer_handler, 0, 0 );

    initialize_uart_irq();

    //video_test();
    //video_line_test();


#if 1
    ee_printf("Initializing USB\n");

    if( USPiInitialize() )
    {
        ee_printf("Initialization OK!\n");
        ee_printf("Checking for keyboards...\n");

        if ( USPiKeyboardAvailable () )
        {
            USPiKeyboardRegisterKeyPressedHandler( _keypress_handler );
            gfx_set_fg(10);
            ee_printf("Keyboard found.\n");
            gfx_set_fg(15);
        }
        else
        {
            gfx_set_fg(9);
            ee_printf("No keyboard found.\n");
            gfx_set_fg(15);
        }
    }

    else ee_printf("USB initialization failed.\n");
#endif

    // DR: Echo terminal geometry
    unsigned int cols, rows, width, height;
    gfx_get_term_size(&rows, &cols);
    gfx_get_resolution(&width, &height);
    ee_printf("Terminal geometry: %dx%d (%dx%dpx)\n", cols, rows, width, height);

    ee_printf("---------\n");
    term_main_loop();
}
