/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

var $cockpit = $cockpit || { };

(function($, $cockpit, cockpit_pages) {

var docker_clients = { };

function get_docker_client(machine) {
    if (!machine)
        machine = cockpit_get_page_param ("machine", "server");
    console.log("DC", machine);
    if (!docker_clients[machine])
        docker_clients[machine] = new DockerClient (machine);
    return docker_clients[machine];
}

function cockpit_quote_cmdline (cmds) {
    function quote(arg) {
        return arg.replace(/\\/g, '\\\\').replace(/ /g, '\\ ');
    }
    return cmds? cmds.map(quote).join(' ') : "";
}

function cockpit_unquote_cmdline (string) {
    function shift(str) {
        return string.replace(/\\ /g, '\u0001').replace(/\\\\/g, '\u0002');
    }
    function unshift(str) {
        return str.replace(/\u0001/g, ' ').replace(/\u0002/g, '\\');
    }

    return shift(string).split(' ').map(unshift);
}

function cockpit_render_container_name (name) {
    if (name.length > 0 && name[0] == "/")
        return name.slice(1);
    else
        return name;
}

function cockpit_render_container_state (state) {
    if (state.Running)
        return F(_("Up since %{StartedAt}"), state);
    else
        return F(_("Exited %{ExitCode}"), state);
}

function multi_line(strings) {
    return strings.map(cockpit_esc).join('<br/>');
}

function insert_table_sorted(table, row) {
    var key = $(row).text();
    var rows = $(table).find("tbody tr");
    for (var j = 0; j < rows.length; j++) {
        if ($(rows[j]).text().localeCompare(key) > 0) {
            $(row).insertBefore(rows[j]);
            row = null;
            break;
        }
    }
    if (row !== null)
        $(table).find("tbody").append(row);
}

PageContainers.prototype = {
    _init: function() {
        this.id = "containers";
    },

    getTitle: function() {
        return C_("page-title", "Containers");
    },

    enter: function(first_visit) {
        var self = this;

        if (first_visit) {
            this.container_filter_btn =
                cockpit_select_btn($.proxy(this, "filter"),
                                   [ { title: _("All"),                 choice: 'all',  is_default: true },
                                     { title: _("Running"),             choice: 'running' }
                                   ]);
            $('#containers-containers .panel-heading span').append(this.container_filter_btn);
        }

        var client = get_docker_client();
        if (client != this.client) {
            if (this.client)
                this.client.off('.containers');

            this.client = client;

            /* HACK: This is our pretend angularjs */
            this.rows = { };
            $('#containers-containers table tbody tr').remove();
            $('#containers-images table tbody tr').remove();

            /* Every time a container appears, disappears, changes */
            $(this.client).on('container.containers', function(event, id, container) {
                self.render_container(id, container);
            });

            /* Every time a image appears, disappears, changes */
            $(this.client).on('image.containers', function(event, id, image) {
                self.render_image(id, image);
            });

            /* High level failures about the overall functionality of docker */
            $(this.client).on('failure.containers', function(event, ex) {
                var msg;
                console.warn(ex);
                if (ex.problem == "not-found")
                    msg = _("Docker is not installed or activated on the system");
                else if (ex.problem == "not-authorized")
                    msg = _("Not authorized to access Docker on this system");
                else
                    msg = ex.toString();
                $("#containers-failure").show();
                $("#containers-failure span").text(msg);
            });
        }

        var id;
        for (id in this.client.containers) {
            this.render_container(id, this.client.containers[id]);
        }

        for (id in this.client.images) {
            this.render_image(id, this.client.images[id]);
        }
    },

    show: function() {
    },

    leave: function() {
    },

    render_container: function(id, container) {
        var self = this;
        var tr = this.rows[id];

        if (!container) {
            if (tr)
                tr.remove();
            delete this.rows[id];
            return;
        }

        var cpuuse, cputext;
        var memuse, memlimit;
        var membar, memtext, memtextstyle;

        if (container.State && container.State.Running) {
            cputext = (container.CpuUsage || 0).toString() + "%";

            memuse = container.MemoryUsage || 0;
            memlimit = container.Config && container.Config.Memory;

            var barvalue = memuse.toString();

            if (memlimit)
                barvalue += "/" + memlimit.toString();

            if (memlimit) {
                var parts = $cockpit.format_bytes(memlimit);
                memtext = (memuse? $cockpit.format_bytes(memuse, parts[1])[0] : "?") + " / " + parts.join(" ");
            } else {
                memtext = (memuse? $cockpit.format_bytes(memuse).join(" ") : "?");
            }


            if (memlimit && memuse > 0.9*memlimit)
                membar.addClass("bar-row-danger");

            membar = true;
            memtextstyle = { 'color': 'inherit', 'text-align': 'inherit' };
        } else {
            membar = false;
            memtext = _("Stopped");
            memtextstyle = { 'color': 'grey', 'text-align': 'right' };
            barvalue = 0;
        }

        var added = false;
        if (!tr) {
            var btn_play = $('<button class="btn btn-default btn-control btn-play">').
                on("click", function() {
                    self.client.start(id).
                        fail(function(ex) {
                            cockpit_show_unexpected_error (ex);
                        });
                    return false;
                });
            var btn_stop = $('<button class="btn btn-default btn-control btn-stop">').
                on("click", function() {
                    self.client.stop(id).
                        fail(function(ex) {
                            cockpit_show_unexpected_error (ex);
                        });
                    return false;
                });
            tr = $('<tr>').append(
                $('<td>'),
                $('<td>'),
                $('<td>'),
                $('<td>'),
                $('<td>').append($cockpit.BarRow("containers-containers")),
                $('<td>'),
                $('<td class="cell-buttons">').append(btn_play, btn_stop))[0];
            $(tr).on('click', function(event) {
                cockpit_go_down ({ page: 'container-details',
                    id: id
                });
            });

            added = true;
            this.rows[id] = tr;
        }

        var row = $(tr).children("td");
        $(row[0]).text(cockpit_render_container_name(container.Name));
        $(row[1]).text(container.Image);
        $(row[2]).text(container.Command);
        $(row[3]).text(cputext);
        $(row[4]).children("div").
            attr("value", barvalue).
            toggle(membar);
        $(row[5]).
            css(memtextstyle).
            text(memtext);
        $(row[6]).children("button.btn-play").toggle(!container.State.Running);
        $(row[6]).children("button.btn-stop").toggle(container.State.Running);

        var filter = cockpit_select_btn_selected(this.container_filter_btn);
        $(tr).toggleClass("unimportant", !container.State.Running);

        if (added)
            insert_table_sorted($('#containers-containers table'), tr);
    },

    render_image: function(id, image) {
        var self = this;
        var tr = this.rows[id];

        if (!image) {
            if (tr)
                tr.remove();
            delete this.rows[id];
            return;
        }

        var added = false;
        if (!tr) {
            var button = $('<button class="btn btn-default btn-control btn-play">').
                on("click", function() {
                    PageRunImage.display(self.client, id);
                    return false;
                });
            tr = $('<tr>').append(
                    $('<td>'),
                    $('<td>'),
                    $('<td>').append($cockpit.BarRow("container-images")),
                    $('<td>'),
                    $('<td class="cell-buttons">').append(button))[0];
            $(tr).on('click', function(event) {
                cockpit_go_down ({ page: 'image-details',
                    id: id
                });
            });

            added = true;
            this.rows[id] = tr;
        }

        var row = $(tr).children("td");
        $(row[0]).html(multi_line(image.RepoTags));
        $(row[1]).text(new Date(image.Created * 1000).toLocaleString());
        $(row[2]).children("div").attr("value", image.VirtualSize);
        $(row[3]).text($cockpit.format_bytes(image.VirtualSize, 1024).join(" "));

        if (added)
            insert_table_sorted($('#containers-images table'), tr);
    },

    filter: function() {
        var filter = cockpit_select_btn_selected(this.container_filter_btn);
        if (filter == "running")
            $("#containers-containers table").addClass("filter-unimportant");
        else
            $("#containers-conatiners table").removeClass("filter-unimportant");
    }

};

function PageContainers() {
    this._init();
}

cockpit_pages.push(new PageContainers());

PageRunImage.prototype = {
    _init: function() {
        this.id = "containers_run_image_dialog";
    },

    getTitle: function() {
        return C_("page-title", "Run Image");
    },

    show: function() {
    },

    leave: function() {
    },

    enter: function(first_visit) {
        if (first_visit) {
            $("#containers-run-image-run").on('click', $.proxy(this, "run"));
        }

        // from https://github.com/dotcloud/docker/blob/master/pkg/namesgenerator/names-generator.go

        var left = [ "happy", "jolly", "dreamy", "sad", "angry", "pensive", "focused", "sleepy", "grave", "distracted", "determined", "stoic", "stupefied", "sharp", "agitated", "cocky", "tender", "goofy", "furious", "desperate", "hopeful", "compassionate", "silly", "lonely", "condescending", "naughty", "kickass", "drunk", "boring", "nostalgic", "ecstatic", "insane", "cranky", "mad", "jovial", "sick", "hungry", "thirsty", "elegant", "backstabbing", "clever", "trusting", "loving", "suspicious", "berserk", "high", "romantic", "prickly", "evil" ];

        var right = [ "lovelace", "franklin", "tesla", "einstein", "bohr", "davinci", "pasteur", "nobel", "curie", "darwin", "turing", "ritchie", "torvalds", "pike", "thompson", "wozniak", "galileo", "euclid", "newton", "fermat", "archimedes", "poincare", "heisenberg", "feynman", "hawking", "fermi", "pare", "mccarthy", "engelbart", "babbage", "albattani", "ptolemy", "bell", "wright", "lumiere", "morse", "mclean", "brown", "bardeen", "brattain", "shockley" ];

        function make_name() {
            function ranchoice(array) {
                return array[Math.round(Math.random() * (array.length-1))];
            }
            return ranchoice(left) + "_" + ranchoice(right);
        }

        $("#containers-run-image-name").val(make_name());
        $("#containers-run-image-command").val(cockpit_quote_cmdline(PageRunImage.image_info.config.Cmd));
        $("#containers-run-image-memory").val("");
        $("#containers-run-image-swap").val("");

        function render_port(p) {
            var port_input = $('<input class="form-control" style="display:inline;width:auto" >');
            var tr =
                $('<tr class="port-map">').append(
                    $('<td>').text(
                        F(_("Bind port %{port} to "),
                          { port: p })),
                    $('<td>').append(
                        port_input));

            port_input.attr('placeholder', _("none"));
            return tr;
        }

        var table = $('#containers_run_image_dialog .modal-body table');
        table.find('.port-map').remove();
        this.port_items = { };
        for (var p in PageRunImage.image_info.config.ExposedPorts) {
            var tr = render_port(p);
            this.port_items[p] = tr;
            table.append(tr);
        }
    },

    run: function() {
        var name = $("#containers-run-image-name").val();
        var cmd = $("#containers-run-image-command").val();
        var mem_limit = cockpit_parse_bytes($("#containers-run-image-memory").val(), 0);
        var swap_limit = cockpit_parse_bytes($("#containers-run-image-swap").val(), 0);
        var port_bindings = { };
        var p, map;
        for (p in this.port_items) {
            map = this.port_items[p].find('input').val();
            if (map)
                port_bindings[p] = [ { "HostIp": "",
                                       "HostPort": map
                                     }
                                   ];
        }

        $("#containers_run_image_dialog").modal('hide');

        PageRunImage.client.post("/containers/create",
                                 { "name": name
                                 },
                                 { "Cmd": cockpit_unquote_cmdline(cmd),
                                   "Image": PageRunImage.image_info.id,
                                   "Memory": mem_limit,
                                   "MemorySwap": swap_limit
                                 },
                                 function (error, result) {
                                     if (error)
                                         cockpit_show_unexpected_error (error);
                                     else {
                                         PageRunImage.client.start(result.Id, { "PortBindings": port_bindings }).
                                                fail(function(ex) {
                                                    cockpit_show_unexpected_error(ex);
                                                });
                                     }
                                 });
    }
};

PageRunImage.display = function(client, id) {
    PageRunImage.image_info = client.images[id];
    PageRunImage.client = client;
    $("#containers_run_image_dialog").modal('show');
};

function PageRunImage() {
    this._init();
}


cockpit_pages.push(new PageRunImage());

PageContainerDetails.prototype = {
    _init: function() {
        this.id = "container-details";
    },

    getTitle: function() {
        return this.name;
    },

    show: function() {
    },

    leave: function() {
        $(this.client).off('.container-details');
    },

    enter: function(first_visit) {
        var self = this;

        if (first_visit) {
            $('#container-details-start').on('click', $.proxy(this, "start_container"));
            $('#container-details-stop').on('click', $.proxy(this, "stop_container"));
            $('#container-details-restart').on('click', $.proxy(this, "restart_container"));
            $('#container-details-delete').on('click', $.proxy(this, "delete_container"));
        }

        this.client = get_docker_client();
        this.container_id = cockpit_get_page_param('id');
        this.name = this.container_id.slice(0,12);

        $(this.client).on('container.container-details', function (event, id, container) {
            if (id == self.container_id)
                self.update();
        });

        this.update();
    },

    update: function() {
        $('#container-details-names').text("");
        $('#container-details-id').text("");
        $('#container-details-created').text("");
        $('#container-details-image').text("");
        $('#container-details-command').text("");
        $('#container-details-state').text("");
        $('#container-details-ports').text("");

        var info = this.client.containers[this.container_id];

        if (!info) {
            $('#container-details-names').text(_("Not found"));
            return;
        }

        var name = cockpit_render_container_name(info.Name);
        if (name != this.name) {
            this.name = name;
            cockpit_content_update_loc_trail();
        }

        var port_bindings = [ ];
        if (info.NetworkSettings) {
            for (var p in info.NetworkSettings.Ports) {
                var h = info.NetworkSettings.Ports[p];
                if (!h)
                    continue;
                for (var i = 0; i < h.length; i++) {
                    port_bindings.push(F(_("%{hip}:%{hport} -> %{cport}"),
                                         { hip: h[i].HostIp,
                                           hport: h[i].HostPort,
                                           cport: p
                                         }));
                }
            }
        }

        $('#container-details-id').text(info.ID);
        $('#container-details-names').text(cockpit_render_container_name(info.Name));
        $('#container-details-created').text(info.Created);
        $('#container-details-image').text(info.Image);
        $('#container-details-command').text(info.Command);
        $('#container-details-state').text(cockpit_render_container_state(info.State));
        $('#container-details-ports').html(port_bindings.map(cockpit_esc).join('<br/>'));
    },

    start_container: function () {
        this.client.start(this.container_id).
                fail(function(ex) {
                    cockpit_show_unexpected_error (ex);
                });
    },

    stop_container: function () {
        this.client.stop(this.container_id).
                fail(function(ex) {
                    cockpit_show_unexpected_error (ex);
                });
    },

    restart_container: function () {
        this.client.restart(this.container_id).
                fail(function(ex) {
                    cockpit_show_unexpected_error (ex);
                });
    },

    delete_container: function () {
        this.client.delete_("/containers/" + this.container_id, function (error, result) {
            if (error)
                cockpit_show_unexpected_error (error);
            else
                cockpit_go_up();
        });
    }

};

function PageContainerDetails() {
    this._init();
}

cockpit_pages.push(new PageContainerDetails());

PageImageDetails.prototype = {
    _init: function() {
        this.id = "image-details";
    },

    getTitle: function() {
        return this.name;
    },

    show: function() {
    },

    leave: function() {
        $(this.client).off('.container-details');
    },

    enter: function(first_visit) {
        var self = this;

        if (first_visit) {

            $('#image-details-run').on('click', $.proxy(this, "run_image"));
            $('#image-details-delete').on('click', $.proxy(this, "delete_image"));
        }

        this.client = get_docker_client();
        this.image_id = cockpit_get_page_param('id');
        this.name = F(_("Image %{id}"), { id: this.image_id.slice(0,12) });

        $(this.client).on('image.image-details', function (event, id, imaege) {
            if (id == self.image_id)
                self.update();
        });

        this.update();
    },

    update: function() {
        $('#image-details-id').text("");
        $('#image-details-entrypoint').text("");
        $('#image-details-command').text("");
        $('#image-details-created').text("");
        $('#image-details-author').text("");
        $('#image-details-ports').text("");

        var info = this.client.images[this.image_id];

        if (!info) {
            $('#image-details-id').text(_("Not found"));
            return;
        }

        if (info.RepoTags && info.RepoTags.length > 0) {
            var name = info.RepoTags[0];
            if (name != this.name) {
                this.name = name;
                cockpit_content_update_loc_trail();
            }
        }

        $('#image-details-id').text(info.id);
        $('#image-details-tags').html(multi_line(info.RepoTags));
        $('#image-details-created').text(info.created);
        $('#image-details-author').text(info.author);

        var config = info.config;
        if (config) {
            var ports = [ ];
            for (var p in config.ExposedPorts) {
                ports.push(p);
            }

            $('#image-details-entrypoint').text(cockpit_quote_cmdline(config.Entrypoint));
            $('#image-details-command').text(cockpit_quote_cmdline(config.Cmd));
            $('#image-details-ports').text(ports.join(', '));
        }
    },

    run_image: function () {
        PageRunImage.display(this.client, this.image_id);
    },

    delete_image: function () {
        this.client.delete_("/images/" + this.image_id, function (error, result) {
            if (error)
                cockpit_show_unexpected_error (error);
            else
                cockpit_go_up();
        });
    }

};

function PageImageDetails() {
    this._init();
}

cockpit_pages.push(new PageImageDetails());


function DockerClient(machine) {
    var me = this;
    var rest = $cockpit.rest("unix:///var/run/docker.sock", machine);

    var events = rest.get("/events");
    var alive = true;

    /* This is a named function because we call it recursively */
    function connect_events() {

        /* Trigger the event signal when JSON from /events */
        events.stream(function(resp) {
            console.log("DockerClient event:", resp);
            $(me).trigger("event");

        }).

        /* Reconnect to /events when it disconnects/fails */
        always(function() {
            window.setTimeout(function() {
                if (alive && events) {
                    events = events.restart();
                    connect_events();
                    alive = false;
                }
            }, 1000);
        });
    }
    connect_events();

    /* All active poll requests for containers/images indexed by Id */
    var polls = { };

    /*
     * Exposed API, all containers and images
     * Contains the combined /container/json and /container/xxx/json
     * output indexed by Id (err id).
     *
     * Same for images
     */
    this.containers = { };
    this.images = { };

    /*
     * Gets a list of the containers and details for each one.  We use
     * /events for notification when something changes.  However, for
     * extra robustness and to account for the fact that there are no
     * events when new images appear, we also poll for changes.
     */
    var containers_meta = { };
    rest.poll("/containers/json", 4000, events, { "all": 1 }).
        stream(function(containers) {
            alive = true;

            /*
             * The output we get from /containers/json is mostly useless
             * conflicting with the information that we get about specific
             * containers. So just use it to get a list of containers.
             */
            $(containers).each(function(i, item) {
                var id = item.Id;
                if (id && !polls[id]) {
                    containers_meta[id] = item;
                    polls[id] = rest.poll("/containers/" + id + "/json", 5000, events).
                        stream(function(container) {
                            $.extend(container, containers_meta[id]);
                            me.containers[id] = container;
                            $(me).trigger("container", [id, container]);
                        }).
                        fail(function(ex) {
                            /*
                             * A 404 is the way we determine when a container
                             * actually goes away
                             */
                            if (ex.status == 404) {
                                delete me.containers[id];
                                $(me).trigger("container", [id, undefined]);
                            }
                        }).
                        always(function() {
                            /*
                             * This lets us start a new poll for this, if it failed
                             * for a reason other than a 404
                             */
                            polls[id].cancel();
                            delete polls[id];
                        });
                }
            });
        }).
        fail(function(ex) {
            $(me).trigger("failure", [ex]);
        });

    /*
     * Gets a list of images and keeps it up to date. Again, the /images/json and
     * /images/xxxx/json have completely inconsistent keys. So using the former
     * is pretty useless here :S
     */
    var images_meta = { };
    var images_req = rest.poll("/images/json", 1000).
        stream(function(images) {
            alive = true;

            $(images).each(function(i, item) {
                var id = item.Id;
                if (id && !polls[id]) {
                    images_meta[id] = item;
                    polls[id] = rest.poll("/images/" + id + "/json", 0, images_req).
                        stream(function(image) {
                            $.extend(image, images_meta[id]);
                            me.images[id] = image;
                            $(me).trigger("image", [id, image]);
                        }).
                        fail(function(ex) {
                            /*
                             * A 404 is the way we determine when a container
                             * actually goes away
                             */
                            if (ex.status == 404) {
                                delete me.images[id];
                                $(me).trigger("image", [id, undefined]);
                            }
                        }).
                        always(function() {
                            /*
                             * This lets us start a new poll for image, if it failed
                             * for a reason other than a 404.
                             */
                            polls[id].cancel();
                            delete polls[id];
                        });
                }
            });
        }).
        fail(function(ex) {
            $(me).trigger("failure", [ex]);
        });

    /* We listen to the resource monitor and include the measurements
     * in the container objects.
     *
     * TODO: Don't assume that the D-Bus client is ready.  Call
     * GetSamples for quicker initialization.
     */

    var dbus_client = cockpit_get_dbus_client (machine);
    var monitor = dbus_client.lookup ("/com/redhat/Cockpit/LxcMonitor",
                                      "com.redhat.Cockpit.MultiResourceMonitor");

    if (monitor) {
        $(monitor).on('NewSample', function (event, timestampUsec, samples) {
            for (var id in me.containers) {
                var container = me.containers[id];
                var sample = samples["lxc/" + id] || samples["docker-" + id + ".slice"];

                var mem = sample? sample[0] : 0;
                var cpu = sample? sample[4] : 0;
                if (mem != container.MemoryUsage || cpu != container.CpuUsage) {
                    container.MemoryUsage = mem;
                    container.CpuUsage = cpu;
                    $(me).trigger("container", [id, container]);
                }
            }
        });
    } else {
        console.log("No monitor");
    }

    this.start = function start(id, options) {
        return rest.post("/containers/" + id + "/start", null, options);
    };

    this.stop = function stop(id, timeout) {
        if (timeout === undefined)
            timeout = 10;
        return rest.post("/containers/" + id + "/stop", { 't': timeout });
    };

    this.restart = function restart(id) {
        return rest.post("/containers/" + id + "/restart");
    };

    /*
     * TODO: it would probably make sense for this API to use
     * Deferreds as well. But for now we just map it to the
     * continuation style API DockerClient has.
     */

    function get(resource, cont) {
        rest.get(resource).done(function(resp) {
                cont(null, resp);
            }).fail(function(reason) {
                cont(reason);
            });
    }

    function post(resource, params, body, cont) {
        rest.post(resource, params, body).done(function(resp) {
                cont(null, resp);
            }).fail(function(reason) {
                cont(reason);
            });
    }

    function delete_ (resource, cont) {
        rest.del(resource).done(function(resp) {
                cont(null, resp);
            }).fail(function(reason) {
                cont(reason);
            });
    }

    this.get = get;
    this.post = post;
    this.delete_ = delete_;
}

})(jQuery, $cockpit, cockpit_pages);
