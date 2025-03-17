-- mod-version:4

local config = require "core.config"
local common = require "core.common"
local core = require "core"

local native = require ".native"
local projectsearch = require "plugins.projectsearch"

config.plugins.projectsearch = common.merge({
  threads = 8
}, config.plugins.projectsearch)

local ResultsView = projectsearch.ResultsView
local old_begin_search = ResultsView.begin_search

function ResultsView:begin_search(path, text, fn)
  if type(fn) == "string" then
    self.search_args = { path, text, fn }
    self.results = {}
    self.last_file_idx = 1
    self.query = text
    self.searching = true
    self.search_started = system.get_time()
    self.selected_idx = 0

    local search = native.init(config.plugins.projectsearch.threads, text, fn, function(file, line, col, text)
      if col < 80 then
        table.insert(self.results, { file = file, text = text, line = line, col = col })
      else
        table.insert(self.results, { file = file, text = "..." .. text, line = line, col = col })
      end
    end)
    core.add_thread(function()
      local i = 1
      for k, project in ipairs(core.projects) do
        for dir_name, file in project:files() do
          if file.type == "file" and (not path or file.filename:find(path, 1, true) == 1) then
            search:find(file.filename)
          end
          self.last_file_idx = i
          i = i + 1
        end
      end
      search:join()
      self.search_total_time = system.get_time() - self.search_started
      self.searching = false
      self.brightness = 100
      core.redraw = true
    end, self.results)

    self.scroll.to.y = 0
  else
    return old_begin_search(self, path, text, fn)
  end
end


local function begin_search(path, text, fn)
  if text == "" then
    core.error("Expected non-empty string")
    return
  end
  local rv = ResultsView(path, text, fn)
  core.root_view:get_active_node_default():add_view(rv)
  return rv
end


function projectsearch.search_plain(text, path, insensitive)
  return begin_search(path, text, insensitive and "insensitive" or "plain")
end
