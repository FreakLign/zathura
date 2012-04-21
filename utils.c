/* See LICENSE file for license and copyright information */

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <math.h>
#include <gtk/gtk.h>
#include <girara/session.h>

#include "utils.h"
#include "zathura.h"
#include "internal.h"
#include "document.h"
#include "page.h"
#include "plugin.h"

#include <girara/datastructures.h>

#define BLOCK_SIZE 64

const char*
file_get_extension(const char* path)
{
  if (!path) {
    return NULL;
  }

  unsigned int i = strlen(path);
  for (; i > 0; i--)
  {
    if (*(path + i) != '.') {
      continue;
    } else {
      break;
    }
  }

  if (!i) {
    return NULL;
  }

  return path + i + 1;
}

bool
file_valid_extension(zathura_t* zathura, const char* path)
{
  if (zathura == NULL || zathura->plugins.manager == NULL || path == NULL) {
    return false;
  }

  const gchar* content_type = g_content_type_guess(path, NULL, 0, NULL);
  if (content_type == NULL) {
    return false;
  }

  zathura_plugin_t* plugin = zathura_plugin_manager_get_plugin(zathura->plugins.manager, content_type);
  g_free((void*)content_type);

  return (plugin == NULL) ? false : true;
}

bool
execute_command(char* const argv[], char** output)
{
  if (!output) {
    return false;
  }

  int p[2];
  if (pipe(p)) {
    return -1;
  }

  pid_t pid = fork();

  if (pid == -1) { // failure
    return false;
  } else if (pid == 0) { // child
    dup2(p[1], 1);
    close(p[0]);

    if (execvp(argv[0], argv) == -1) {
      return false;
    }
  } else { // parent
    dup2(p[0], 0);
    close(p[1]);

    /* read output */
    unsigned int bc = BLOCK_SIZE;
    unsigned int i  = 0;
    char* buffer    = malloc(sizeof(char) * bc);
    *output = NULL;

    if (!buffer) {
      close(p[0]);
      return false;
    }

    char c;
    while (1 == read(p[0], &c, 1)) {
      buffer[i++] = c;

      if (i == bc) {
        bc += BLOCK_SIZE;
        char* tmp = realloc(buffer, sizeof(char) * bc);

        if (!tmp) {
          free(buffer);
          close(p[0]);
          return false;
        }

        buffer = tmp;
      }
    }

    char* tmp = realloc(buffer, sizeof(char) * (bc + 1));
    if (!tmp) {
      free(buffer);
      close(p[0]);
      return false;
    }

    buffer = tmp;
    buffer[i] = '\0';

    *output = buffer;

    /* wait for child to terminate */
    waitpid(pid, NULL, 0);
    close(p[0]);
  }

  return true;
}

void
document_index_build(GtkTreeModel* model, GtkTreeIter* parent,
    girara_tree_node_t* tree)
{
  girara_list_t* list        = girara_node_get_children(tree);
  GIRARA_LIST_FOREACH(list, girara_tree_node_t*, iter, node)
    zathura_index_element_t* index_element = (zathura_index_element_t*)girara_node_get_data(node);

    gchar* description = NULL;
    if (index_element->type == ZATHURA_LINK_TO_PAGE) {
      description = g_strdup_printf("Page %d", index_element->target.page_number);
    } else {
      description = g_strdup(index_element->target.uri);
    }

    GtkTreeIter tree_iter;
    gtk_tree_store_append(GTK_TREE_STORE(model), &tree_iter, parent);
    gtk_tree_store_set(GTK_TREE_STORE(model), &tree_iter, 0, index_element->title, 1, description, 2, index_element, -1);
    g_object_weak_ref(G_OBJECT(model), (GWeakNotify) zathura_index_element_free, index_element);
    g_free(description);

    if (girara_node_get_num_children(node) > 0) {
      document_index_build(model, &tree_iter, node);
    }

  GIRARA_LIST_FOREACH_END(list, gchar*, iter, name);
}

void
page_calculate_offset(zathura_t* zathura, zathura_page_t* page, page_offset_t* offset)
{
  g_return_if_fail(page != NULL);
  g_return_if_fail(offset != NULL);
  GtkWidget* widget = zathura_page_get_widget(zathura, page);

  g_return_if_fail(gtk_widget_translate_coordinates(widget,
    zathura->ui.page_widget, 0, 0, &(offset->x), &(offset->y)) == true);
}

zathura_rectangle_t rotate_rectangle(zathura_rectangle_t rectangle, unsigned int degree, int height, int width)
{
  zathura_rectangle_t tmp;
  switch (degree) {
    case 90:
      tmp.x1 = height - rectangle.y2;
      tmp.x2 = height - rectangle.y1;
      tmp.y1 = rectangle.x1;
      tmp.y2 = rectangle.x2;
      break;
    case 180:
      tmp.x1 = width - rectangle.x2;
      tmp.x2 = width - rectangle.x1;
      tmp.y1 = height - rectangle.y2;
      tmp.y2 = height - rectangle.y1;
      break;
    case 270:
      tmp.x1 = rectangle.y1;
      tmp.x2 = rectangle.y2;
      tmp.y1 = width - rectangle.x2;
      tmp.y2 = width - rectangle.x1;
      break;
    default:
      tmp.x1 = rectangle.x1;
      tmp.x2 = rectangle.x2;
      tmp.y1 = rectangle.y1;
      tmp.y2 = rectangle.y2;
  }

  return tmp;
}

zathura_rectangle_t
recalc_rectangle(zathura_page_t* page, zathura_rectangle_t rectangle)
{
  if (page == NULL) {
    goto error_ret;
  }

  zathura_document_t* document = zathura_page_get_document(page);

  if (document == NULL) {
    goto error_ret;
  }

  double page_height = zathura_page_get_height(page);
  double page_width  = zathura_page_get_width(page);
  double scale       = zathura_document_get_scale(document);

  zathura_rectangle_t tmp;

  switch (zathura_document_get_rotation(document)) {
    case 90:
      tmp.x1 = (page_height - rectangle.y2) * scale;
      tmp.x2 = (page_height - rectangle.y1) * scale;
      tmp.y1 = rectangle.x1 * scale;
      tmp.y2 = rectangle.x2 * scale;
      break;
    case 180:
      tmp.x1 = (page_width  - rectangle.x2) * scale;
      tmp.x2 = (page_width  - rectangle.x1) * scale;
      tmp.y1 = (page_height - rectangle.y2) * scale;
      tmp.y2 = (page_height - rectangle.y1) * scale;
      break;
    case 270:
      tmp.x1 = rectangle.y1 * scale;
      tmp.x2 = rectangle.y2 * scale;
      tmp.y1 = (page_width - rectangle.x2) * scale;
      tmp.y2 = (page_width - rectangle.x1) * scale;
      break;
    default:
      tmp.x1 = rectangle.x1 * scale;
      tmp.x2 = rectangle.x2 * scale;
      tmp.y1 = rectangle.y1 * scale;
      tmp.y2 = rectangle.y2 * scale;
  }

  return tmp;

error_ret:

  return rectangle;
}

void
set_adjustment(GtkAdjustment* adjustment, gdouble value)
{
  gtk_adjustment_set_value(adjustment, MAX(gtk_adjustment_get_lower(adjustment),
        MIN(gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), value)));
}

void
page_calc_height_width(zathura_page_t* page, unsigned int* page_height, unsigned int* page_width, bool rotate)
{
  g_return_if_fail(page != NULL && page_height != NULL && page_width != NULL);

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return;
  }

  double height = zathura_page_get_height(page);
  double width  = zathura_page_get_width(page);
  double scale  = zathura_document_get_scale(document);

  if (rotate && zathura_document_get_rotation(document) % 180) {
    *page_width  = ceil(height * scale);
    *page_height = ceil(width  * scale);
  } else {
    *page_width  = ceil(width  * scale);
    *page_height = ceil(height * scale);
  }
}

GtkWidget*
zathura_page_get_widget(zathura_t* zathura, zathura_page_t* page)
{
  if (zathura == NULL || page == NULL || zathura->pages == NULL) {
    return NULL;
  }

  unsigned int page_number = zathura_page_get_index(page);

  return zathura->pages[page_number];
}

void
readjust_view_after_zooming(zathura_t *zathura, float old_zoom) {
  if (zathura == NULL || zathura->document == NULL) {
    return;
  }

  GtkScrolledWindow *window = GTK_SCROLLED_WINDOW(zathura->ui.session->gtk.view);
  GtkAdjustment* vadjustment = gtk_scrolled_window_get_vadjustment(window);
  GtkAdjustment* hadjustment = gtk_scrolled_window_get_hadjustment(window);

  double scale = zathura_document_get_scale(zathura->document);
  gdouble valx = gtk_adjustment_get_value(hadjustment) / old_zoom * scale;
  gdouble valy = gtk_adjustment_get_value(vadjustment) / old_zoom * scale;
  set_adjustment(hadjustment, valx);
  set_adjustment(vadjustment, valy);
}

