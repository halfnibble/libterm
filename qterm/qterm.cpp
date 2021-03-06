#include <QPainter>
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDesktopWidget>
#include <QPaintEvent>
#include <QFontMetrics>
#include <QTextStream>
#include <QScrollArea>
#include <qterm.h>
#include <stdio.h>
#include <QKeyEvent>
#include <QTimer>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>
#ifdef __QNX__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <bps/bps.h>
#endif
#include "term_logging.h"

#define BLINK_SPEED 1000
#define HEIGHT 100

#define ON_CURSOR(x,y) (cursor_on && cursor_x == x && cursor_y == y)

#define PIEKEYS     1

QTerm::QTerm(QWidget *parent) : QWidget(parent)
{
    init();
}

QTerm::QTerm(QWidget *parent, term_t terminal) : QWidget(parent)
{
    this->terminal = terminal;
    init();
}

void QTerm::init()
{
    std::vector<Qt::Key> keys;
    char_width = 0;
    char_height = 0;
    cursor_on = 1;
    piekey_active = 0;
    workaround = false;
    keys.push_back(Qt::Key_Up);
    keys.push_back(Qt::Key_Escape);
    keys.push_back(Qt::Key_Right);
    keys.push_back(Qt::Key_AsciiTilde);
    keys.push_back(Qt::Key_Down);
    keys.push_back(Qt::Key_Bar);
    keys.push_back(Qt::Key_Left);
    keys.push_back(Qt::Key_Tab);
    piekeyboard = new QPieKeyboard(this);
    piekeyboard->initialize( PIEKEYS, keys );

    term_set_user_data( terminal, this );
    term_register_update( terminal, term_update );
    term_register_cursor( terminal, term_update_cursor );
    term_register_bell( terminal, term_bell );
    notifier = new QSocketNotifier( term_get_file_descriptor(terminal), QSocketNotifier::Read );

#if !defined(__MACH__) && !defined(__QNX__)
    // Not supported on OSX
    exit_notifier = new QSocketNotifier( term_get_file_descriptor(terminal), QSocketNotifier::Exception );
    QObject::connect(exit_notifier, SIGNAL(activated(int)), this, SLOT(terminate()));
#endif

    cursor_timer = new QTimer( this );
    QObject::connect(notifier, SIGNAL(activated(int)), this, SLOT(terminal_data()));
    QObject::connect(cursor_timer, SIGNAL(timeout()), this, SLOT(blink_cursor()));

    // Setup the initial font

#ifdef __QNX__
    font = new QFont("Andale Mono");
#else
    font = new QFont();
    font->setStyleHint(QFont::Monospace);
    font->setStyleStrategy(QFont::NoAntialias);
    font->setFamily("Monospace");
    font->setFixedPitch(true);
    font->setKerning(false);
#endif
    if( QApplication::desktop()->screenGeometry().width() < 1000 ) {
        font->setPointSize(6);
    } else {
        font->setPointSize(12);
    }

    // Workaround for a bug in OSX - Dave reports that maxWidth returns 0,
    // when width of different characters returns the correct value
    QFontMetrics metrics(*font);
    if(metrics.maxWidth() == 0) {
        fontWorkAround = true;
    } else {
        fontWorkAround = false;
    }

    char_width = metrics.width(QChar(' '));
    char_height = metrics.lineSpacing();
    char_descent = metrics.descent();

    QObject::connect(piekeyboard, SIGNAL(keypress(Qt::Key)), this, SLOT(keypress(Qt::Key)));
    cursor_timer->start(BLINK_SPEED);
    setAttribute(Qt::WA_AcceptTouchEvents);
}

QTerm::~QTerm()
{
    delete notifier;
    delete exit_notifier;
    delete font;
    delete piekeyboard;
    term_free( terminal );
}

void QTerm::resize_term()
{
    int visible_height;
    visible_height = minimumSize().height() / char_height;
    slog("resize term! %d %d %d -> (%dx%d)", size().width(), size().height(), char_width, size().width()/char_width, HEIGHT);
    resize(size().width(), HEIGHT * char_height);
    scrollback_height = HEIGHT - visible_height;
    term_resize( terminal, size().width() / char_width, visible_height, scrollback_height );
    // Workaround for a Qt bug - missing a redraw after resizing
    workaround = true;
    cursor_timer->stop();
    cursor_timer->start(1);
}

void QTerm::term_bell(term_t handle)
{
#ifdef __QNX__
    char command[] = "msg::play_sound\ndat:json:{\"sound\":\"notification_general\"}";
    int f = open("/pps/services/multimedia/sound/control", O_RDWR);
    write(f, command, sizeof(command));
    ::close(f);
#else
    QApplication::beep();
#endif
}

void QTerm::term_update(term_t handle, int x, int y, int width, int height)
{
    QTerm *term = (QTerm *)term_get_user_data( handle );

    term->update(x, y, width, height);
}

void QTerm::term_update_cursor(term_t handle, int old_x, int old_y, int new_x, int new_y)
{
    QTerm *term = (QTerm *)term_get_user_data( handle );

    // Reset cursor blink
    term->cursor_on = 1;
    term->cursor_timer->stop();
    term->cursor_timer->start(BLINK_SPEED);

    // Update old and new cursor location
    term->update_grid( old_x, old_y, 1, 1);
    term->update_grid( new_x, new_y, 1, 1);
}

void QTerm::keypress(Qt::Key key)
{
    // TODO - Merge this with keyPressEvent
    switch(key) {
        case Qt::Key_CapsLock:
        case Qt::Key_Shift:
            break;
        case Qt::Key_Tab:
            term_send_data( terminal, "\t", 1 );
            break;
        case Qt::Key_Return:
            term_send_data( terminal, "\n", 1 );
            break;
        case Qt::Key_Backspace:
            term_send_data( terminal, "\b", 1 );
            break;
        case Qt::Key_Up:
            term_send_special( terminal, TERM_KEY_UP );
            break;
        case Qt::Key_Down:
            term_send_special( terminal, TERM_KEY_DOWN );
            break;
        case Qt::Key_Right:
            term_send_special( terminal, TERM_KEY_RIGHT );
            break;
        case Qt::Key_Left:
            term_send_special( terminal, TERM_KEY_LEFT );
            break;
        default:
            // Treat as ascii and hope for the best. Works in a lot of cases
            {
                char c = (char)key;
                term_send_data( terminal, &c, 1 );
            }
            break;
    }
}

void QTerm::resizeRequest(QSize size)
{
    setMinimumSize(size);
    size.setHeight(QWIDGETSIZE_MAX);
    setMaximumSize(size);
    resize_term();
}

void QTerm::terminal_data()
{
    if( !term_process_child( terminal ) ) {
        exit(0);
    }
}

void QTerm::terminate()
{
    exit(0);
}

void QTerm::blink_cursor()
{
    int cursor_x, cursor_y;

    cursor_on ^= 1;
    term_get_cursor_pos( terminal, &cursor_x, &cursor_y );
    update_grid( cursor_x, cursor_y, 1, 1);
    if( workaround ) {
        // Workaround for Qt bug
        update(0, 0, size().width() / char_width, minimumSize().height() / char_height);
        workaround = false;
        cursor_timer->stop();
        cursor_timer->start(BLINK_SPEED);
    }
}

void QTerm::update(int grid_x_min,
                   int grid_y_min,
                   int grid_width,
                   int grid_height)
{
    update_grid(grid_x_min, grid_y_min, grid_width, grid_height);

    // Notify parent of update to allow scrolling
    emit gridUpdated();
}

// Called to update the grid.
// Region is based on grid coordinates.
void QTerm::update_grid(int grid_x_min,
                        int grid_y_min,
                        int grid_width,
                        int grid_height)
{
    int coords_x_min, coords_y_min;
    int coords_x_max, coords_y_max;

    if (grid_x_min < 0 || grid_y_min < 0) {
        return;
    }
    if ( fontWorkAround ) {
        // If fontWorkAround is set, we cannot trust the characters to 
        // be monospaced.  So we need to calculate the coordinates based
        // on the string lengths of the strings in the grid.
        QFontMetrics metrics(*font);

        const char *str;
        int i;

        coords_x_min = width();
        coords_x_max = 0;
        for (i= grid_y_min; i < (grid_y_min + grid_height); i++) {
            int tmp;
            str = term_get_line( terminal, i );
                
            // X limits
            tmp = metrics.width(QString(str),grid_x_min);
            if (tmp < coords_x_min) {
                coords_x_min = tmp;
            }
            tmp = metrics.width(QString(str),grid_x_min + grid_width);
            if (tmp > coords_x_max) {
                coords_x_max = tmp;
            }
        }
        
    } else {
        coords_x_min = grid_x_min * char_width;
        coords_x_max = grid_width * char_width;
    }
    coords_y_min = (grid_y_min + scrollback_height) * char_height;
    coords_y_max = grid_height * char_height;
    QWidget::update(coords_x_min, coords_y_min,
                    coords_x_max, coords_y_max);
}

// Returns bool if the string, located at grid location X,Y, will be within
// the updateRect.
void QTerm::getRenderedStringRect( const QString string,
                                   int attrib,
                                   QFont *pFont,
                                   QRect *pUpdateRect) 
{
    QFontMetrics *pFontMetrics = NULL;
    QFont *pTmpFont;
    
    if (pFont == NULL) {
        pTmpFont = font;
    } else {
        pTmpFont = pFont;
    }

    pTmpFont->setUnderline( attrib & TERM_ATTRIB_UNDERSCORE );
    pFontMetrics = new QFontMetrics( *pTmpFont );

    pUpdateRect->setWidth( pFontMetrics->width( string ));
    pUpdateRect->setHeight( char_height );
    
    delete pFontMetrics;

}

void QTerm::paintEvent(QPaintEvent *event)
{
    int i;
    const char *str;
    int cursor_x_coord;
    QPainter painter(this);
    QColor fgColor(255,255,255);
    QColor bgColor(0,0,0);
    int gridWidth, gridHeight;
    int cursor_x, cursor_y;
    const uint32_t *const *colors;
    const uint32_t *const *attribs;

    painter.setBackgroundMode(Qt::TransparentMode);
    painter.setBrush(QColor(8, 0, 0));
    painter.setFont( *font );

    // Get grid dimensions
    term_get_grid_size(terminal, &gridWidth, &gridHeight);

    // First erase the grid with its current dimensions
    painter.drawRect(event->rect());
   
    //slog("Rect: (%d, %d) %d x %d", event->rect().x(), event->rect().y(), event->rect().width(), event->rect().height());

    painter.setPen(fgColor);
    painter.setBrush(fgColor);
    term_get_cursor_pos( terminal, &cursor_x, &cursor_y );

    attribs = term_get_attribs( terminal );
    colors = term_get_colours( terminal );

    str = term_get_line( terminal, cursor_y );
    // Workaround to get the cursor in the right spot.  For some
    // reason, on OSX (again), the monospace font does is not really
    // monospace for skinny characters! 
    if (fontWorkAround) {
        cursor_x_coord = painter.fontMetrics().width(QString(str),cursor_x);
    } else {
        cursor_x_coord = cursor_x * char_width;
    }

    if ( cursor_on ) {
       painter.setPen(fgColor);
       painter.setBrush(fgColor);
       painter.drawRect( cursor_x_coord + 1, 
                         (cursor_y + scrollback_height) * char_height + 1,
                         char_width - 2, char_height - 2); 
    }
    painter.setPen(fgColor);
    painter.setBrush(fgColor);

        
    for (i=0; i< HEIGHT;i++) {
        unsigned int currentAttrib; 
        unsigned int currentColor;
        bool currentOnCursor;
        int color=0x00ffffff;
        int chunkStart=0;
        int chunkPos;
        int last_x_pos = 0;
        bool recalcSubString = false;
        QString qString;
        QStringRef subString;
        QRect stringRect;
        QRect intersectedRect;
       
        currentAttrib = attribs[i - scrollback_height][chunkStart];
        currentColor = colors[i - scrollback_height][chunkStart];
        currentOnCursor = ON_CURSOR(0, i);
        color = term_get_fg_color(currentAttrib, currentColor);
        str = term_get_line( terminal, i - scrollback_height );
        qString = qString.append(str);
        painter.setPen( QColor( (color >> 16) & 0xFF,
                                (color >> 8 ) & 0xFF,
                                (color & 0xFF) ) );

        stringRect.setX( 0 );
        stringRect.setY( i * char_height );
        getRenderedStringRect( qString, currentAttrib, NULL, &stringRect);

        intersectedRect = stringRect.intersected( event->rect() );

        if (intersectedRect.height() == 0) { 
            // Don't render this string as it is not in the event's height
            continue;
        }
        intersectedRect.setTop(stringRect.y());

        /* Chunk each string whenever we need to change rendering params */
        for(chunkPos=0; chunkPos< gridWidth; chunkPos++) {
            if( attribs[i - scrollback_height][chunkPos] != currentAttrib ||
                colors[i - scrollback_height][chunkPos] != currentColor ||
                ON_CURSOR(chunkPos, i) != currentOnCursor) {
                // flag to tell outer loop to recalc the substring
                recalcSubString = true;

                subString = qString.midRef( chunkStart, chunkPos-chunkStart );
                // Update the intersected rect with the substring
                getRenderedStringRect( subString.toString(), 
                                      currentAttrib, 
                                      NULL,
                                      &intersectedRect);

                // Render everything up to this point.
                if( currentOnCursor ) {
                    painter.setPen( bgColor );
                } else if( currentAttrib & TERM_ATTRIB_REVERSE ) {
                    color = term_get_fg_color( currentAttrib, currentColor );
                    painter.setPen( QColor( (color >> 16) & 0xFF,
                                            (color >> 8 ) & 0xFF,
                                            (color & 0xFF) ) );
                    painter.drawRect( last_x_pos, intersectedRect.y(),
                                      intersectedRect.width(), intersectedRect.height() );
                    color = term_get_bg_color( currentAttrib, currentColor );
                    painter.setPen( QColor( (color >> 16) & 0xFF,
                                            (color >> 8 ) & 0xFF,
                                            (color & 0xFF) ) );
                } else {
                    color = term_get_fg_color( currentAttrib, currentColor );
                    painter.setPen( QColor( (color >> 16) & 0xFF,
                                            (color >> 8 ) & 0xFF,
                                            (color & 0xFF) ) );
                }

                painter.setFont( *font );

                // need to manually add xpos until we have x-cliping.
                painter.drawText( last_x_pos,
                                 intersectedRect.y(),
                                 stringRect.width(),
                                 intersectedRect.height(),
                                  Qt::TextExpandTabs,
                                  subString.toString() );
                
                // Update the local variables
                last_x_pos += intersectedRect.width();
                intersectedRect.translate( intersectedRect.width(), 0 );
                currentColor = colors[i - scrollback_height][chunkPos];
                currentAttrib = attribs[i - scrollback_height][chunkPos];
                currentOnCursor = ON_CURSOR(chunkPos, i);
                chunkStart=chunkPos;
            }
        }
        // draw whatever remains.
        color = term_get_fg_color(currentAttrib, currentColor);
        painter.setPen( QColor( (color >> 16) & 0xFF,
                                (color >> 8 ) & 0xFF,
                                (color & 0xFF) ) );
        subString = qString.midRef( chunkStart, chunkPos - chunkStart);
        if (recalcSubString) {
            getRenderedStringRect( subString.toString(),
                                   currentAttrib,
                                   NULL,
                                   &intersectedRect );
        }
        painter.setFont( *font );
        painter.drawText( last_x_pos, intersectedRect.y(),
                          stringRect.width(), intersectedRect.height(),
                          Qt::TextExpandTabs,
                          subString.toString(),
                          &intersectedRect );
    }
}

void QTerm::keyPressEvent(QKeyEvent *event)
{
    switch(event->key()) {
        // FIXME These first four are workarounds for bugs in QT. Remove once it is fixed
        case Qt::Key_CapsLock:
        case Qt::Key_Shift:
            break;
        case Qt::Key_Return:
            term_send_data( terminal, "\n", 1 );
            break;
        case Qt::Key_Backspace:
            term_send_data( terminal, "\b", 1 );
            break;
        case Qt::Key_Escape:
            term_send_data( terminal, "\x1B", 1 );
            break;
        case Qt::Key_Up:
            term_send_special( terminal, TERM_KEY_UP );
            break;
        case Qt::Key_Down:
            term_send_special( terminal, TERM_KEY_DOWN );
            break;
        case Qt::Key_Right:
            term_send_special( terminal, TERM_KEY_RIGHT );
            break;
        case Qt::Key_Left:
            term_send_special( terminal, TERM_KEY_LEFT );
            break;
        default:
            term_send_data( terminal, event->text().toUtf8().constData(), event->text().count() );
            break;
    }
}

bool QTerm::event(QEvent *event)
{
    QList<QTouchEvent::TouchPoint> touchPoints;

    switch(event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            touchPoints = static_cast<QTouchEvent *>(event)->touchPoints();

            switch(event->type()) {
                case QEvent::TouchBegin:
                    return true;
                case QEvent::TouchUpdate:
                    if( touchPoints.length() == PIEKEYS ) {
                        if( !piekey_active ) {
                            piekey_active = 1;
                            for(int i = 0; i < PIEKEYS; i ++ ) {
                                piekeyboard->activate(i, touchPoints[i].pos().x(), touchPoints[i].pos().y());
                            }
                        }
                        for(int i = 0; i < PIEKEYS; i ++ ) {
                            piekeyboard->moveTouch(i, touchPoints[i].pos().x(), touchPoints[i].pos().y());
                        }
                    } else {
                        if( piekey_active ) {
                            piekeyboard->release();
                            piekey_active = 0;
                        }
                    }
                    return true;
                case QEvent::TouchEnd:
                    if( piekey_active ) {
                        piekeyboard->release();
                        piekey_active = 0;
                        return true;
                    }
                    // Workaround for qt bug - it doesn't do the refresh properly when the piekey is released in some cases
                    QWidget::update(0, 0, width(), height());
                default:
                    break;
            }
            break;
        default:
            return QWidget::event(event);
    }

    return false;
}
