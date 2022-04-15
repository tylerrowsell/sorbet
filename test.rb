
class Foo
  def hi
    puts "hi"
  end
end

def make_hi
  :hi
end

[Foo.new].each(&make_hi)
